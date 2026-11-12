#include "low_latency_hold.h"
#include "protected_pointer.h"
#include "xmx_status.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

namespace xmx::low_latency
{
namespace
{
using SetModeFn = HRESULT(STDMETHODCALLTYPE*)(LowLatencyDevice*, BOOL, BOOL, UINT32);

HRESULT STDMETHODCALLTYPE HookedSetLatencySleepMode(LowLatencyDevice* self, BOOL mode, BOOL boost, UINT32 intervalUs);

std::mutex gLock;
IUnknown* gObserved = nullptr;
LowLatencyDevice* gTarget = nullptr;
uintptr_t gSlot = 0;
std::atomic<SetModeFn> gOriginal{nullptr};
std::optional<Request> gLast;
bool gHold = false;

const char* OnOff(bool on) noexcept
{
    return on ? "on" : "off";
}

std::string Describe(Request request)
{
    return std::string("mode ") + OnOff(request.mode) + " boost " + OnOff(request.boost)
        + " interval " + std::to_string(request.intervalUs) + " us";
}

std::string Result(HRESULT result)
{
    return SUCCEEDED(result) ? std::string() : " (result " + std::to_string(static_cast<long>(result)) + ")";
}

HRESULT ForwardLocked(Request request)
{
    return gOriginal.load(std::memory_order_acquire)(gTarget, request.mode, request.boost, request.intervalUs);
}

void ResyncLocked(const char* why)
{
    const auto request = Resync(gLast, gHold);
    if (!request)
    {
        Log(std::string("low latency hold off: ") + why + ", no request was recorded, so the mode returns at the "
            "game's next request");
        return;
    }
    const HRESULT result = ForwardLocked(*request);
    Log(std::string("low latency hold ") + OnOff(gHold) + ": " + why + ", forwarded "
        + Describe(*request) + Result(result));
}

LowLatencyDevice* QueryLowLatency(IUnknown* device) noexcept
{
    LowLatencyDevice* found = nullptr;
    if (FAILED(device->QueryInterface(kIidLowLatencyDevice, reinterpret_cast<void**>(&found)))) return nullptr;
    return found;
}

bool PatchSlotLocked(LowLatencyDevice* device) noexcept
{
    void** const table = *reinterpret_cast<void***>(device);
    const uintptr_t slot = reinterpret_cast<uintptr_t>(&table[kSetLatencySleepModeSlot]);
    void* const current = protected_pointer::ReadPointer(slot);
    void* const hook = reinterpret_cast<void*>(&HookedSetLatencySleepMode);
    if (gSlot == slot) return current == hook;
    if (gSlot != 0 || current == hook) return false;
    gOriginal.store(reinterpret_cast<SetModeFn>(current), std::memory_order_release);
    const auto published = protected_pointer::ReplaceProtectedPointer(slot, reinterpret_cast<uintptr_t>(current),
        reinterpret_cast<uintptr_t>(hook), &VirtualProtect, PAGE_READWRITE);
    if (!published.replacementWasPublished)
    {
        gOriginal.store(nullptr, std::memory_order_release);
        return false;
    }
    gSlot = slot;
    return published.disposition == protected_pointer::PublishDisposition::ePublishedRestored;
}

HRESULT STDMETHODCALLTYPE HookedSetLatencySleepMode(LowLatencyDevice* self, BOOL mode, BOOL boost, UINT32 intervalUs)
{
    const SetModeFn original = gOriginal.load(std::memory_order_acquire);
    try
    {
        std::lock_guard lock(gLock);
        const Request requested{mode != FALSE, boost != FALSE, intervalUs};
        const Request sent = Forwarded(requested, gHold);
        const bool news = !gLast || *gLast != requested;
        gLast = requested;
        if (news && sent != requested)
            Log("low latency hold: the game asked for " + Describe(requested) + ", forwarded " + Describe(sent));
        return original(self, sent.mode, sent.boost, sent.intervalUs);
    }
    catch (...)
    {
        return original(self, mode, boost, intervalUs);
    }
}

void InstallLocked(IUnknown* device)
{
    LowLatencyDevice* const found = QueryLowLatency(device);
    if (!found)
    {
        Log("low latency hook not available: the game's D3D12 device has no ID3DLowLatencyDevice");
        return;
    }
    const bool firstPatch = gSlot == 0;
    if (!PatchSlotLocked(found))
    {
        found->Release();
        Log("low latency hook not available: SetLatencySleepMode could not be patched");
        return;
    }
    if (gTarget) gTarget->Release();
    gTarget = found;
    if (firstPatch) Log("low latency hook installed on the game's ID3DLowLatencyDevice::SetLatencySleepMode");
    if (gHold) ResyncLocked("the hook was installed");
}
}

void Observe(IUnknown* device) noexcept
{
    try
    {
        std::lock_guard lock(gLock);
        if (device == nullptr || device == gObserved) return;
        gObserved = device;
        InstallLocked(device);
    }
    catch (...) {}
}

bool Configure(bool hold) noexcept
{
    try
    {
        std::lock_guard lock(gLock);
        if (gHold == hold) return false;
        gHold = hold;
        if (gTarget) ResyncLocked(hold ? "applied live" : "released live");
        else Log(std::string("low latency hold ") + OnOff(hold) + ": waiting for the game's device");
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool Hooked() noexcept
{
    try
    {
        std::lock_guard lock(gLock);
        return gTarget != nullptr;
    }
    catch (...)
    {
        return false;
    }
}

bool Holding() noexcept
{
    try
    {
        std::lock_guard lock(gLock);
        return gHold;
    }
    catch (...)
    {
        return false;
    }
}
}
