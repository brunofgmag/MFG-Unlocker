#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "xmx/xmx_latency.h"
#include "xmx/xmx_status.h"

namespace
{
unsigned gChecks = 0, gFailures = 0;
std::vector<std::string> gLog;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

struct Marked
{
    uint32_t id;
    xell_latency_marker_type_t marker;
};

struct Fakes
{
    unsigned creates = 0, destroys = 0, modes = 0, sleeps = 0;
    xell_result_t createResult = XELL_RESULT_SUCCESS, modeResult = XELL_RESULT_SUCCESS,
        sleepResult = XELL_RESULT_SUCCESS, markerResult = XELL_RESULT_SUCCESS;
    bool lastLowLatency = false;
    ID3D12Device* lastDevice = nullptr;
    xell_context_handle_t lastContext = nullptr;
    uint32_t lastSleepId = 0;
    std::vector<Marked> marked;
    std::vector<xell_context_handle_t> destroyed;
};

Fakes gFakes;
char gContexts[8];

ID3D12Device* Device(uintptr_t value)
{
    return reinterpret_cast<ID3D12Device*>(value);
}

xell_result_t FakeCreate(ID3D12Device* device, xell_context_handle_t* out)
{
    ++gFakes.creates;
    gFakes.lastDevice = device;
    if (gFakes.createResult != XELL_RESULT_SUCCESS) return gFakes.createResult;
    *out = reinterpret_cast<xell_context_handle_t>(&gContexts[gFakes.creates % 8]);
    return XELL_RESULT_SUCCESS;
}

xell_result_t FakeSetSleepMode(xell_context_handle_t context, const xell_sleep_params_t* params)
{
    ++gFakes.modes;
    gFakes.lastContext = context;
    gFakes.lastLowLatency = params->bLowLatencyMode == 1 && params->minimumIntervalUs == 0;
    return gFakes.modeResult;
}

xell_result_t FakeSleep(xell_context_handle_t context, uint32_t id)
{
    ++gFakes.sleeps;
    gFakes.lastContext = context;
    gFakes.lastSleepId = id;
    return gFakes.sleepResult;
}

xell_result_t FakeAddMarker(xell_context_handle_t, uint32_t id, xell_latency_marker_type_t marker)
{
    gFakes.marked.push_back({id, marker});
    return gFakes.markerResult;
}

xell_result_t FakeDestroy(xell_context_handle_t context)
{
    ++gFakes.destroys;
    gFakes.destroyed.push_back(context);
    return XELL_RESULT_SUCCESS;
}

xmx::latency::Functions FakeFunctions()
{
    return {&FakeCreate, &FakeSetSleepMode, &FakeSleep, &FakeAddMarker, &FakeDestroy};
}

void Fresh()
{
    xmx::latency::DrainLine();
    gFakes = Fakes{};
    gLog.clear();
    xmx::latency::Attach(FakeFunctions());
}

size_t Untraced()
{
    size_t count = 0;
    for (const auto& line : gLog) count += !line.starts_with("XeLL trace ");
    return count;
}

size_t Traced()
{
    return gLog.size() - Untraced();
}

bool Logged(const std::string& text)
{
    for (const auto& line : gLog)
        if (line == text) return true;
    return false;
}

std::atomic<bool> gSleepEntered{false}, gSleepRelease{false}, gSleepReturned{false}, gDestroyAfterSleep{false};
std::atomic<unsigned> gBlockedDestroys{0};

xell_result_t BlockingSleep(xell_context_handle_t, uint32_t)
{
    gSleepEntered = true;
    while (!gSleepRelease) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    gSleepReturned = true;
    return XELL_RESULT_SUCCESS;
}

xell_result_t DestroyNotingSleep(xell_context_handle_t)
{
    gDestroyAfterSleep = gSleepReturned.load();
    ++gBlockedDestroys;
    return XELL_RESULT_SUCCESS;
}

bool WaitFor(const std::atomic<bool>& flag)
{
    for (int waited = 0; waited < 5000 && !flag; ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return flag;
}

void DestroyWaitsForSleep()
{
    Fresh();
    xmx::latency::Functions blocking = FakeFunctions();
    blocking.sleep = &BlockingSleep;
    blocking.destroy = &DestroyNotingSleep;
    xmx::latency::Attach(blocking);
    xmx::latency::ObserveDevice(Device(0xD000));
    Check(xmx::latency::Ready(), "a context exists for the sleep to run on");
    gBlockedDestroys = 0;

    bool slept = false;
    std::thread sleeper([&] { slept = xmx::latency::Sleep(500); });
    if (!Check(WaitFor(gSleepEntered), "the sleep is in flight")) gSleepRelease = true;
    std::atomic<bool> observed{false};
    std::thread observer([&] {
        xmx::latency::ObserveDevice(Device(0xD100));
        observed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check(!observed && gBlockedDestroys == 0, "a device change does not destroy the context under a sleep in flight");
    gSleepRelease = true;
    sleeper.join();
    observer.join();
    Check(slept, "the sleep in flight returns normally");
    Check(gBlockedDestroys == 1 && gDestroyAfterSleep, "the context is destroyed after the sleep returned");
    Check(xmx::latency::Ready(), "the new device has its own context");
}

void NothingBeforeAttach()
{
    Check(!xmx::latency::Ready(), "not ready before anything is attached");
    xmx::latency::Marker(XELL_SIMULATION_START, 10);
    Check(!xmx::latency::Sleep(10), "Sleep before attach returns false");
    xmx::latency::ObserveDevice(Device(0x1000));
    Check(!xmx::latency::Ready() && gFakes.creates == 0, "a device before attach creates nothing");
    Check(gFakes.marked.empty() && gFakes.sleeps == 0, "Marker and Sleep before attach reach no function");
    Check(xmx::latency::DrainLine().empty(), "no line without a context");
}

void AttachedWithoutDevice()
{
    Fresh();
    Check(!xmx::latency::Ready(), "attached without a device is not ready");
    xmx::latency::ObserveDevice(nullptr);
    Check(!xmx::latency::Ready() && gFakes.creates == 0, "a null device creates nothing");
    xmx::latency::Marker(XELL_SIMULATION_START, 10);
    Check(!xmx::latency::Sleep(10), "Sleep without a context returns false");
    Check(gFakes.marked.empty() && gFakes.sleeps == 0, "Marker and Sleep without a context reach no function");
}

void SameDeviceTwice()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0x2000));
    xmx::latency::ObserveDevice(Device(0x2000));
    Check(xmx::latency::Ready(), "a device makes it ready");
    Check(gFakes.creates == 1 && gFakes.lastDevice == Device(0x2000), "the same device twice gives one create");
    Check(gFakes.modes == 1 && gFakes.lastLowLatency, "low-latency mode is set once, with no interval");
    Check(gFakes.destroys == 0, "nothing is destroyed for the same device");
    Check(Logged("XeLL context created"), "the creation is logged");
    Check(Untraced() == 1, "the creation is logged once");
}

void SecondDevice()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0x3000));
    const auto first = gFakes.lastContext;
    const unsigned destroysBefore = gFakes.destroys;
    xmx::latency::ObserveDevice(Device(0x3008));
    Check(gFakes.creates == 2 && gFakes.lastDevice == Device(0x3008), "a second device creates a second context");
    Check(gFakes.destroys == destroysBefore + 1 && gFakes.destroyed.back() == first, "the first context is destroyed");
    Check(xmx::latency::Ready(), "the second context is ready");
}

void CreateFailure()
{
    Fresh();
    gFakes.createResult = XELL_RESULT_ERROR_UNSUPPORTED_DEVICE;
    xmx::latency::ObserveDevice(Device(0x4000));
    xmx::latency::ObserveDevice(Device(0x4000));
    Check(!xmx::latency::Ready(), "a create failure leaves it not ready");
    Check(!xmx::latency::Sleep(5) && gFakes.sleeps == 0, "Sleep returns false without a context");
    Check(gFakes.creates == 1, "the failed device is not retried on every call");
    Check(Logged("XeLL context failed: create result -1") && Untraced() == 1, "the failure is logged once with its step");
    Check(xmx::latency::DrainLine().empty(), "no line after a failed create");
}

void SleepModeFailure()
{
    Fresh();
    gFakes.modeResult = XELL_RESULT_ERROR_DEVICE;
    xmx::latency::ObserveDevice(Device(0x5000));
    Check(!xmx::latency::Ready(), "a sleep-mode failure leaves it not ready");
    Check(gFakes.destroys == 1, "the half-made context is destroyed");
    Check(Logged("XeLL context failed: sleep mode result -6"), "the failure names its step");
}

void MarkersAndIds()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0x6000));
    for (uint32_t marker = XELL_SIMULATION_START; marker <= XELL_PRESENT_END; ++marker)
        xmx::latency::Marker(marker, 100);
    Check(gFakes.marked.size() == 6, "the six marker types are forwarded");
    bool sameId = true, types = true;
    for (size_t index = 0; index < gFakes.marked.size(); ++index)
    {
        sameId &= gFakes.marked[index].id == gFakes.marked[0].id;
        types &= gFakes.marked[index].marker == static_cast<xell_latency_marker_type_t>(index);
    }
    Check(sameId && gFakes.marked[0].id != 0, "one token keeps one id");
    Check(types, "the types arrive as the game sent them");
    const uint32_t firstId = gFakes.marked[0].id;
    xmx::latency::Marker(XELL_PRESENT_END + 1, 101);
    xmx::latency::Marker(0xFFFFFFFFu, 101);
    Check(gFakes.marked.size() == 6, "a type above present end is ignored");
    xmx::latency::Marker(XELL_SIMULATION_START, 101);
    xmx::latency::Marker(XELL_SIMULATION_START, 102);
    Check(firstId == 100 && gFakes.marked[6].id == 101 && gFakes.marked[7].id == 102, "the ids are the game's tokens");
    xmx::latency::Marker(XELL_RENDERSUBMIT_END, 100);
    Check(gFakes.marked.size() == 9 && gFakes.marked[8].id == 100, "an earlier frame's marker keeps its token");
    xmx::latency::Marker(XELL_SIMULATION_START, 0);
    Check(gFakes.marked.size() == 9, "a marker on token 0 is dropped");
}

void SleepUsesTheTokensId()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0x7000));
    xmx::latency::Marker(XELL_SIMULATION_START, 200);
    xmx::latency::Marker(XELL_SIMULATION_START, 201);
    Check(xmx::latency::Sleep(201), "Sleep succeeds for a live token");
    Check(gFakes.lastSleepId == gFakes.marked[1].id && gFakes.sleeps == 1, "Sleep passes the id the markers used");
    Check(xmx::latency::Sleep(200) && gFakes.lastSleepId == gFakes.marked[0].id, "an earlier token keeps its own id");
    xmx::latency::Marker(XELL_SIMULATION_START, 199);
    Check(xmx::latency::Sleep(0) && gFakes.lastSleepId == 202,
        "a sleep on token 0 is for the frame after the latest simulation start");
    xmx::latency::ObserveDevice(Device(0x7100));
    Check(!xmx::latency::Sleep(0) && gFakes.sleeps == 3, "a sleep on token 0 before any simulation start does not sleep");
}

void SleepFailure()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0x8000));
    xmx::latency::Marker(XELL_SIMULATION_START, 300);
    gFakes.sleepResult = XELL_RESULT_ERROR_INVALID_CONTEXT;
    Check(!xmx::latency::Sleep(300), "an erroring sleep makes Sleep false");
    gFakes.sleepResult = XELL_RESULT_SUCCESS;
    Check(xmx::latency::Sleep(300), "a later good sleep is true");
    const std::string line = xmx::latency::DrainLine();
    Check(line.find("sleeps=1 failed=1") != std::string::npos, "the failure is counted apart from the good sleep");
}

void SleepFailureCode()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0xA000));
    xmx::latency::Marker(XELL_SIMULATION_START, 500);
    gFakes.sleepResult = XELL_RESULT_ERROR_INVALID_ARGUMENT;
    xmx::latency::Sleep(501);
    xmx::latency::Sleep(502);
    Check(Logged("XeLL sleep failed on id 501 after marker id 500 result -4"),
        "a failing sleep logs its result, its id and the last marker id");
    Check(Untraced() == 2, "the same result is logged once");
    gFakes.sleepResult = XELL_RESULT_ERROR_UNINITIALIZED;
    xmx::latency::Sleep(503);
    Check(Untraced() == 3 && gLog.back().ends_with(" result -3"), "a different result is logged again");
}

void MarkerFailure()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0xB000));
    gFakes.markerResult = XELL_RESULT_ERROR_INVALID_ARGUMENT;
    xmx::latency::Marker(XELL_SIMULATION_START, 600);
    xmx::latency::Marker(XELL_SIMULATION_END, 600);
    const uint32_t id = gFakes.marked[0].id;
    Check(Logged("XeLL marker 0 failed on id " + std::to_string(id) + " result -4"), "a failing marker logs its result");
    Check(Untraced() == 2, "the same marker result is logged once");
    Check(xmx::latency::DrainLine().ends_with("markerFailed=2"), "failed markers are counted");
}

void TraceIsBounded()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0xC000));
    xmx::latency::Marker(XELL_SIMULATION_START, 700, "reflex");
    Check(Logged("XeLL trace reflex marker 0 token 700 id 700 result 0"),
        "a marker is traced with its source, token and id");
    xmx::latency::Sleep(0);
    Check(Logged("XeLL trace sleep token 0 id 701 result 0"), "a sleep is traced with its token and id");
    xmx::latency::Marker(XELL_SIMULATION_START, 0);
    Check(Logged("XeLL trace pcl marker 0 token 0 id 0 dropped"), "a dropped marker is traced");
    for (uint32_t token = 702; token < 900; ++token) xmx::latency::Marker(XELL_SIMULATION_START, token);
    Check(Traced() == 96, "the trace stops after 96 events");
    xmx::latency::ObserveDevice(Device(0xC100));
    xmx::latency::Marker(XELL_SIMULATION_START, 900);
    Check(Traced() == 97, "a new context traces again");
}

void DrainLine()
{
    Fresh();
    xmx::latency::ObserveDevice(Device(0x9000));
    xmx::latency::Marker(XELL_SIMULATION_START, 400);
    xmx::latency::Marker(XELL_SIMULATION_END, 400);
    xmx::latency::Marker(XELL_SIMULATION_END, 401);
    xmx::latency::Marker(XELL_RENDERSUBMIT_START, 400);
    xmx::latency::Marker(XELL_RENDERSUBMIT_END, 400);
    xmx::latency::Marker(XELL_PRESENT_START, 401);
    xmx::latency::Marker(XELL_PRESENT_START, 402);
    xmx::latency::Marker(XELL_PRESENT_END, 402);
    xmx::latency::Marker(99, 402);
    xmx::latency::Sleep(403);
    xmx::latency::Sleep(404);
    const std::string expected = "XeLL 10 s: markers sim=1/2 submit=1/1 present=2/1 sleeps=2 failed=0 markerFailed=0";
    Check(xmx::latency::DrainLine() == expected, "the line reports the counts");
    Check(xmx::latency::DrainLine()
            == "XeLL 10 s: markers sim=0/0 submit=0/0 present=0/0 sleeps=0 failed=0 markerFailed=0",
        "the counts reset after a drain");
}

int Fakes_()
{
    NothingBeforeAttach();
    AttachedWithoutDevice();
    SameDeviceTwice();
    SecondDevice();
    CreateFailure();
    SleepModeFailure();
    MarkersAndIds();
    SleepUsesTheTokensId();
    SleepFailure();
    SleepFailureCode();
    MarkerFailure();
    TraceIsBounded();
    DrainLine();
    DestroyWaitsForSleep();
    return 0;
}

int Runtime(const std::wstring& folder)
{
    const HMODULE ll = LoadLibraryExW((folder + L"\\libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Check(ll != nullptr, "the XeSS SDK's libxell.dll loads")) return 1;
    xmx::latency::Functions functions{};
    Check(xmx::latency::Resolve(ll, functions), "Resolve finds the five exports");
    Check(functions.create && functions.setSleepMode && functions.sleep && functions.addMarker && functions.destroy,
        "the five pointers are not null");
    xmx::latency::Functions other{};
    Check(!xmx::latency::Resolve(GetModuleHandleW(L"kernel32.dll"), other), "a module without them is refused");
    Check(!xmx::latency::Resolve(nullptr, other), "no module is refused");
    if (gFailures) return 1;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const HRESULT created = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    std::printf("D3D12CreateDevice at feature level 11_0 returned 0x%08lx\n", static_cast<unsigned long>(created));
    if (FAILED(created)) return 2;
    xmx::latency::Attach(functions);
    xmx::latency::ObserveDevice(device.Get());
    Check(xmx::latency::Ready(), "a real XeLL context is created on a real device");
    for (uint32_t token = 1; token <= 3; ++token)
    {
        const bool slept = xmx::latency::Sleep(token);
        std::printf("real xellSleep on token %u: %s\n", token, slept ? "slept" : "failed");
        for (uint32_t marker = XELL_SIMULATION_START; marker <= XELL_PRESENT_END; ++marker)
            xmx::latency::Marker(marker, token);
    }
    const std::string line = xmx::latency::DrainLine();
    std::printf("%s\n", line.c_str());
    Check(line.find("markerFailed=0") != std::string::npos, "the real context accepts the six markers");

    const uint32_t first = 97920;
    xmx::latency::Marker(XELL_SIMULATION_START, first);
    xmx::latency::Marker(XELL_SIMULATION_END, first);
    xmx::latency::Marker(XELL_RENDERSUBMIT_START, first);
    xmx::latency::Sleep(0);
    for (uint32_t frame = first + 1; frame <= first + 12; ++frame)
    {
        xmx::latency::Marker(XELL_SIMULATION_START, frame);
        xmx::latency::Marker(XELL_RENDERSUBMIT_END, frame - 1);
        xmx::latency::Marker(XELL_SIMULATION_END, frame);
        xmx::latency::Marker(XELL_PRESENT_START, frame - 1);
        xmx::latency::Marker(XELL_PRESENT_END, frame - 1);
        xmx::latency::Marker(XELL_RENDERSUBMIT_START, frame);
        xmx::latency::Sleep(0);
    }
    const std::string msfs = xmx::latency::DrainLine();
    std::printf("MSFS order: %s\n", msfs.c_str());
    Check(msfs.ends_with("sleeps=13 failed=0 markerFailed=0"),
        "MSFS 2024's order, one frame overlapped and every sleep on token 0, is accepted by the real XeLL");
    return 0;
}
}

namespace xmx
{
void Log(const std::string& text) noexcept
{
    gLog.push_back(text);
    std::printf("log: %s\n", text.c_str());
}

void LogSummary(const std::string& text) noexcept
{
    Log(text);
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    int code = 1;
    if (mode == L"fakes") code = Fakes_();
    else if (mode == L"runtime" && argc > 2) code = Runtime(argv[2]);
    else std::printf("FAIL unknown mode\n");
    std::printf("XMX_LATENCY checks=%u failures=%u\n", gChecks, gFailures);
    if (code == 2) return 2;
    return gFailures || gChecks == 0 || code ? 1 : 0;
}
