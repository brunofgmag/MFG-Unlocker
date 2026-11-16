#include "xmx_latency.h"
#include "xmx_status.h"
#include <array>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace xmx::latency
{
namespace
{
constexpr size_t kMarkerTypes = XELL_PRESENT_END + 1;

struct Counts
{
    std::array<uint64_t, kMarkerTypes> markers{};
    uint64_t sleeps = 0, failed = 0, markerFailed = 0;
};

std::shared_mutex gContextLife;
std::mutex gLock;
Functions gFunctions{};
bool gAttached = false;
xell_context_handle_t gContext = nullptr;
ID3D12Device* gDevice = nullptr;
uint32_t gLatestSimulation = 0;
Counts gCounts;
xell_result_t gLastSleepFailure = XELL_RESULT_SUCCESS;
xell_result_t gLastMarkerFailure = XELL_RESULT_SUCCESS;
uint32_t gLastMarkerId = 0;
struct Limit
{
    uint32_t screenFps = 0;
    uint32_t presentedPerFrame = 1;
    uint32_t intervalUs = 0;
};

Limit gWanted;
uint32_t gAppliedIntervalUs = 0;
xell_result_t gLastIntervalFailure = XELL_RESULT_SUCCESS;
constexpr uint32_t kTraceEvents = 96;
uint32_t gTraced = 0;

template <typename Function>
bool Export(HMODULE ll, const char* name, Function& out) noexcept
{
    out = reinterpret_cast<Function>(GetProcAddress(ll, name));
    return out != nullptr;
}

void Fail(const char* step, xell_result_t result)
{
    Log(std::string("XeLL context failed: ") + step + " result " + std::to_string(static_cast<int>(result)));
}

void NoteFailureLocked(xell_result_t& last, xell_result_t result, const std::string& text)
{
    if (result == last) return;
    last = result;
    Log(text + " result " + std::to_string(static_cast<int>(result)));
}

bool TracingLocked() noexcept
{
    return gTraced < kTraceEvents;
}

void TraceLocked(const std::string& what, uint32_t token, uint32_t id, const std::string& outcome)
{
    ++gTraced;
    LogSummary("XeLL trace " + what + " token " + std::to_string(token) + " id " + std::to_string(id) + " "
        + outcome);
}

std::string MarkerName(const char* source, uint32_t marker)
{
    return std::string(source) + " marker " + std::to_string(marker);
}

std::string Outcome(xell_result_t result)
{
    return "result " + std::to_string(static_cast<int>(result));
}

bool DeviceChangeLocked(ID3D12Device* device) noexcept
{
    return device != nullptr && device != gDevice;
}

void DestroyLocked() noexcept
{
    if (gContext) gFunctions.destroy(gContext);
    gContext = nullptr;
}

uint32_t IntervalUs(uint32_t screenFps, uint32_t presentedPerFrame) noexcept
{
    if (!screenFps) return 0;
    const uint64_t numerator = 1000000ull * presentedPerFrame;
    return static_cast<uint32_t>((numerator + screenFps - 1) / screenFps);
}

std::string LimitText(const Limit& limit)
{
    if (!limit.intervalUs) return "XeLL frame limit off";
    return "XeLL frame limit " + std::to_string(limit.intervalUs) + " us (" + std::to_string(limit.screenFps)
        + " FPS on screen at " + std::to_string(limit.presentedPerFrame) + "x)";
}

xell_result_t SetSleepModeLocked(xell_context_handle_t context, uint32_t intervalUs) noexcept
{
    xell_sleep_params_t mode{};
    mode.bLowLatencyMode = 1;
    mode.minimumIntervalUs = intervalUs;
    return gFunctions.setSleepMode(context, &mode);
}

void ApplyWantedIntervalLocked()
{
    if (gAppliedIntervalUs == gWanted.intervalUs) return;
    const xell_result_t result = SetSleepModeLocked(gContext, gWanted.intervalUs);
    if (result != XELL_RESULT_SUCCESS)
        return NoteFailureLocked(gLastIntervalFailure, result, LimitText(gWanted) + " failed");
    gAppliedIntervalUs = gWanted.intervalUs;
    gLastIntervalFailure = XELL_RESULT_SUCCESS;
    Log(LimitText(gWanted));
}

void CreateLocked(ID3D12Device* device)
{
    xell_context_handle_t context = nullptr;
    xell_result_t result = gFunctions.create(device, &context);
    if (result != XELL_RESULT_SUCCESS || !context)
        return Fail("create", result == XELL_RESULT_SUCCESS ? XELL_RESULT_ERROR_UNKNOWN : result);
    result = SetSleepModeLocked(context, gWanted.intervalUs);
    if (result != XELL_RESULT_SUCCESS)
    {
        gFunctions.destroy(context);
        return Fail("sleep mode", result);
    }
    gContext = context;
    gAppliedIntervalUs = gWanted.intervalUs;
    gLastIntervalFailure = XELL_RESULT_SUCCESS;
    gLatestSimulation = 0;
    gLastSleepFailure = XELL_RESULT_SUCCESS;
    gLastMarkerFailure = XELL_RESULT_SUCCESS;
    gLastMarkerId = 0;
    gTraced = 0;
    Log("XeLL context created");
    if (gAppliedIntervalUs) Log(LimitText(gWanted));
}
}

bool Resolve(HMODULE ll, Functions& out) noexcept
{
    if (ll == nullptr) return false;
    Functions found{};
    const bool all = Export(ll, "xellD3D12CreateContext", found.create)
        && Export(ll, "xellSetSleepMode", found.setSleepMode) && Export(ll, "xellSleep", found.sleep)
        && Export(ll, "xellAddMarkerData", found.addMarker) && Export(ll, "xellDestroyContext", found.destroy);
    if (all) out = found;
    return all;
}

void Attach(const Functions& functions) noexcept
{
    std::lock_guard lock(gLock);
    gFunctions = functions;
    const bool first = !gAttached;
    gAttached = true;
    if (first && gDevice) CreateLocked(gDevice);
}

void ObserveDevice(ID3D12Device* device) noexcept
{
    try
    {
        {
            std::lock_guard lock(gLock);
            if (!DeviceChangeLocked(device)) return;
        }
        std::unique_lock life(gContextLife);
        std::lock_guard lock(gLock);
        if (!DeviceChangeLocked(device)) return;
        DestroyLocked();
        gDevice = device;
        if (gAttached) CreateLocked(device);
    }
    catch (...) {}
}

void Marker(uint32_t marker, uint32_t token, const char* source) noexcept
{
    if (marker > XELL_PRESENT_END) return;
    try
    {
        std::lock_guard lock(gLock);
        if (!gContext) return;
        const uint32_t id = token;
        if (!id)
        {
            if (TracingLocked()) TraceLocked(MarkerName(source, marker), token, id, "dropped");
            return;
        }
        const xell_result_t result = gFunctions.addMarker(gContext, id, static_cast<xell_latency_marker_type_t>(marker));
        if (TracingLocked()) TraceLocked(MarkerName(source, marker), token, id, Outcome(result));
        ++gCounts.markers[marker];
        gLastMarkerId = id;
        if (result != XELL_RESULT_SUCCESS)
        {
            ++gCounts.markerFailed;
            NoteFailureLocked(gLastMarkerFailure, result, "XeLL marker " + std::to_string(marker) + " failed on id "
                + std::to_string(id));
        }
        if (marker == XELL_SIMULATION_START && id > gLatestSimulation) gLatestSimulation = id;
    }
    catch (...) {}
}

bool Attached() noexcept
{
    try
    {
        std::lock_guard lock(gLock);
        return gAttached;
    }
    catch (...)
    {
        return false;
    }
}

bool Ready() noexcept
{
    try
    {
        std::lock_guard lock(gLock);
        return gContext != nullptr;
    }
    catch (...)
    {
        return false;
    }
}

bool Sleep(uint32_t token) noexcept
{
    try
    {
        xell_context_handle_t context = nullptr;
        uint32_t id = 0;
        decltype(Functions::sleep) sleep = nullptr;
        std::shared_lock life(gContextLife);
        {
            std::lock_guard lock(gLock);
            if (!gContext) return false;
            id = token ? token : gLatestSimulation ? gLatestSimulation + 1 : 0;
            if (!id)
            {
                if (TracingLocked()) TraceLocked("sleep", token, id, "dropped");
                return false;
            }
            ApplyWantedIntervalLocked();
            context = gContext;
            sleep = gFunctions.sleep;
        }
        const xell_result_t result = sleep(context, id);
        life.unlock();
        const bool slept = result == XELL_RESULT_SUCCESS;
        std::lock_guard lock(gLock);
        ++(slept ? gCounts.sleeps : gCounts.failed);
        if (TracingLocked()) TraceLocked("sleep", token, id, Outcome(result));
        if (!slept)
            NoteFailureLocked(gLastSleepFailure, result, "XeLL sleep failed on id " + std::to_string(id)
                + " after marker id " + std::to_string(gLastMarkerId));
        return slept;
    }
    catch (...)
    {
        return false;
    }
}

void SetFrameLimit(uint32_t screenFps, uint32_t presentedPerFrame) noexcept
{
    const uint32_t perFrame = presentedPerFrame ? presentedPerFrame : 1;
    try
    {
        std::lock_guard lock(gLock);
        gWanted = {screenFps, perFrame, IntervalUs(screenFps, perFrame)};
    }
    catch (...) {}
}

std::string DrainLine()
{
    std::lock_guard lock(gLock);
    if (!gContext) return {};
    const Counts counts = std::exchange(gCounts, Counts{});
    char line[256];
    std::snprintf(line, sizeof(line),
        "XeLL 10 s: markers sim=%llu/%llu submit=%llu/%llu present=%llu/%llu sleeps=%llu failed=%llu "
        "markerFailed=%llu",
        static_cast<unsigned long long>(counts.markers[XELL_SIMULATION_START]),
        static_cast<unsigned long long>(counts.markers[XELL_SIMULATION_END]),
        static_cast<unsigned long long>(counts.markers[XELL_RENDERSUBMIT_START]),
        static_cast<unsigned long long>(counts.markers[XELL_RENDERSUBMIT_END]),
        static_cast<unsigned long long>(counts.markers[XELL_PRESENT_START]),
        static_cast<unsigned long long>(counts.markers[XELL_PRESENT_END]),
        static_cast<unsigned long long>(counts.sleeps), static_cast<unsigned long long>(counts.failed),
        static_cast<unsigned long long>(counts.markerFailed));
    return line;
}
}
