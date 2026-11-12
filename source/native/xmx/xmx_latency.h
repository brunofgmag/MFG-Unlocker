#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <xell.h>
#include <xell_d3d12.h>
#include <cstdint>
#include <string>
#include "low_latency_hold.h"

namespace xmx::latency
{
struct Functions
{
    decltype(&xellD3D12CreateContext) create;
    decltype(&xellSetSleepMode) setSleepMode;
    decltype(&xellSleep) sleep;
    decltype(&xellAddMarkerData) addMarker;
    decltype(&xellDestroyContext) destroy;
};

bool Resolve(HMODULE ll, Functions& out) noexcept;
void Attach(const Functions& functions) noexcept;
void ObserveDevice(ID3D12Device* device) noexcept;
void Marker(uint32_t marker, uint32_t token, const char* source = "pcl") noexcept;
bool Ready() noexcept;
bool Sleep(uint32_t token) noexcept;
std::string DrainLine();
}
