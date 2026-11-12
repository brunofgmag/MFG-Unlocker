#pragma once
#include <Windows.h>
#include <unknwn.h>
#include <cstdint>
#include "low_latency_rules.h"

namespace xmx::low_latency
{
constexpr GUID kIidLowLatencyDevice = {0xf3112584, 0x41f9, 0x348d, {0xa5, 0x9b, 0x00, 0xb7, 0xe1, 0xd2, 0x85, 0xd6}};
constexpr size_t kSetLatencySleepModeSlot = 5;

struct LowLatencyDevice : IUnknown
{
    virtual BOOL STDMETHODCALLTYPE SupportsLowLatency() = 0;
    virtual HRESULT STDMETHODCALLTYPE LatencySleep() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLatencySleepMode(BOOL lowLatencyMode, BOOL lowLatencyBoost,
        UINT32 minimumIntervalUs) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLatencyMarker(UINT64 frameId, UINT32 markerType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLatencyInfo(void* latencyResults) = 0;
};

void Observe(IUnknown* device) noexcept;
bool Configure(bool hold) noexcept;
bool Hooked() noexcept;
bool Holding() noexcept;
}
