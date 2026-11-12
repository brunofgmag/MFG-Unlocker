#include <Windows.h>
#include <unknwn.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xmx/low_latency_hold.h"
#include "xmx/xmx_status.h"

namespace xmx
{
std::vector<std::string> gLines;
void Log(const std::string& text) noexcept
{
    gLines.push_back("XMX " + text);
}
void LogSummary(const std::string& text) noexcept
{
    gLines.push_back(text);
}
}

namespace
{
using xmx::low_latency::LowLatencyDevice;

unsigned gChecks = 0, gFailures = 0;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

struct Call
{
    BOOL mode;
    BOOL boost;
    UINT32 intervalUs;
    const void* self;
};

struct FakeDevice : LowLatencyDevice
{
    std::vector<Call> calls;
    unsigned queries = 0, refs = 1;
    HRESULT result = S_OK;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!IsEqualGUID(iid, xmx::low_latency::kIidLowLatencyDevice)) return E_NOINTERFACE;
        ++queries;
        AddRef();
        *out = static_cast<LowLatencyDevice*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    BOOL STDMETHODCALLTYPE SupportsLowLatency() override { return TRUE; }
    HRESULT STDMETHODCALLTYPE LatencySleep() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetLatencySleepMode(BOOL mode, BOOL boost, UINT32 intervalUs) override
    {
        calls.push_back({mode, boost, intervalUs, this});
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetLatencyMarker(UINT64, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLatencyInfo(void*) override { return S_OK; }
};

struct PlainDevice : IUnknown
{
    unsigned queries = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override
    {
        ++queries;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

HRESULT Ask(FakeDevice& device, BOOL mode, BOOL boost, UINT32 intervalUs)
{
    LowLatencyDevice* volatile game = &device;
    return game->SetLatencySleepMode(mode, boost, intervalUs);
}

size_t TimesLogged(const std::string& text)
{
    size_t times = 0;
    for (const auto& line : xmx::gLines)
        if (line.find(text) != std::string::npos) ++times;
    return times;
}

bool Logged(const std::string& text)
{
    return TimesLogged(text) != 0;
}

bool EveryLineIsXmxLowLatency()
{
    if (xmx::gLines.empty()) return false;
    for (const auto& line : xmx::gLines)
        if (line.rfind("XMX low latency ", 0) != 0) return false;
    return true;
}

bool LastCall(const FakeDevice& device, BOOL mode, BOOL boost, UINT32 intervalUs)
{
    if (device.calls.empty()) return false;
    const Call& call = device.calls.back();
    return call.mode == mode && call.boost == boost && call.intervalUs == intervalUs;
}

void NotAvailable()
{
    PlainDevice device;
    xmx::low_latency::Observe(&device);
    xmx::low_latency::Observe(&device);
    Check(!xmx::low_latency::Hooked(), "a device without ID3DLowLatencyDevice is not hooked");
    Check(device.queries == 1, "the same device is asked once");
    Check(Logged("hook not available: the game's D3D12 device has no ID3DLowLatencyDevice"), "the missing interface is logged");
    Check(xmx::low_latency::Configure(true), "the hold records on without a hook");
    Check(xmx::low_latency::Holding(), "the hold is on");
    Check(Logged("hold on: waiting for the game's device"), "the hold waiting for a device is logged");
    Check(!xmx::low_latency::Configure(true), "the same hold again is no change");
    Check(xmx::low_latency::Configure(false), "the hold records off without a hook");
    Check(!xmx::low_latency::Holding(), "the hold is off");
}

void Forwarding()
{
    FakeDevice device;
    xmx::low_latency::Observe(&device);
    Check(xmx::low_latency::Hooked(), "a device with ID3DLowLatencyDevice is hooked");
    Check(Logged("hook installed"), "the install is logged");
    Check(device.calls.empty(), "installing with the hold off forwards nothing");
    xmx::low_latency::Observe(&device);
    Check(device.queries == 1, "the same device is asked once");

    Ask(device, TRUE, TRUE, 500);
    Check(device.calls.size() == 1 && LastCall(device, TRUE, TRUE, 500), "hold off: the game's request passes unchanged");
    Ask(device, FALSE, TRUE, 0);
    Check(LastCall(device, FALSE, TRUE, 0), "hold off: mode off and boost on pass unchanged");
    Ask(device, TRUE, FALSE, 8333);
    Check(LastCall(device, TRUE, FALSE, 8333), "hold off: mode on, boost off and interval pass unchanged");
    Check(device.calls.back().self == static_cast<LowLatencyDevice*>(&device), "the original gets the game's device");

    device.calls.clear();
    Check(xmx::low_latency::Configure(true), "the hold turns on");
    Check(device.calls.size() == 1 && LastCall(device, FALSE, FALSE, 8333), "turning on re-forwards the last request held");
    Check(Logged("hold on: applied live, forwarded mode off boost off interval 8333 us"), "the hold on is logged");

    Ask(device, TRUE, TRUE, 500);
    Check(device.calls.size() == 2 && LastCall(device, FALSE, FALSE, 500), "hold on: mode and boost are forwarded off, interval stays");
    Ask(device, TRUE, FALSE, 0);
    Check(LastCall(device, FALSE, FALSE, 0), "hold on: mode on, boost off is forwarded off");
    Check(Logged("the game asked for mode on boost on interval 500 us, forwarded mode off boost off interval 500 us"),
        "an altered request is logged");

    device.calls.clear();
    Check(xmx::low_latency::Configure(false), "the hold turns off");
    Check(device.calls.size() == 1 && LastCall(device, TRUE, FALSE, 0), "turning off re-forwards the last request as the game made it");
    Check(Logged("hold off: released live, forwarded mode on boost off interval 0 us"), "the hold off is logged");
    Check(!xmx::low_latency::Configure(false), "the same hold again is no change");
    Check(device.calls.size() == 1, "no change forwards nothing");

    device.result = E_FAIL;
    device.calls.clear();
    xmx::low_latency::Configure(true);
    Check(device.calls.size() == 1 && Logged("forwarded mode off boost off interval 0 us (result"), "a failed re-forward is logged with its result");
    device.result = S_OK;
    Check(Ask(device, TRUE, TRUE, 0) == S_OK, "the original's result returns to the game");
    device.result = E_FAIL;
    Check(Ask(device, TRUE, TRUE, 0) == E_FAIL, "the original's failure returns to the game");
}

void InstallWithHoldOn()
{
    FakeDevice device;
    xmx::low_latency::Configure(true);
    Check(device.calls.empty(), "the hold before any device forwards nothing");
    xmx::low_latency::Observe(&device);
    Check(device.calls.size() == 1 && LastCall(device, FALSE, FALSE, 0), "installing with the hold on forwards mode off, boost off, interval 0");
    Check(Logged("hold on: the hook was installed, forwarded mode off boost off interval 0 us"), "the forward on install is logged");

    device.calls.clear();
    xmx::low_latency::Configure(false);
    Check(device.calls.empty(), "releasing with no recorded request forwards nothing");
    Check(Logged("no request was recorded, so the mode returns at the game's next request"), "the late release is logged");

    Ask(device, TRUE, TRUE, 0);
    Check(LastCall(device, TRUE, TRUE, 0), "the game's next request passes unchanged");
}

void SecondDevice()
{
    FakeDevice first, second;
    xmx::low_latency::Observe(&first);
    xmx::low_latency::Observe(&second);
    Check(xmx::low_latency::Hooked(), "a second device on the same table keeps the hook");
    Check(TimesLogged("hook installed") == 1, "the install is logged once for the shared table");
    Ask(second, TRUE, FALSE, 11);
    Check(second.calls.size() == 1 && first.calls.empty(), "the call reaches the original once, on the device that made it");
    xmx::low_latency::Configure(true);
    Check(second.calls.size() == 2 && LastCall(second, FALSE, FALSE, 11), "the live forward goes to the latest device");
    Check(first.calls.empty(), "the first device gets nothing live");
}
}

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "not-available") NotAvailable();
    else if (mode == "forwarding") Forwarding();
    else if (mode == "install-with-hold-on") InstallWithHoldOn();
    else if (mode == "second-device") SecondDevice();
    else
    {
        std::printf("FAIL unknown mode\n");
        return 1;
    }
    Check(EveryLineIsXmxLowLatency(), "every line starts with XMX low latency, the prefix once");
    std::printf("%u checks, %u failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
