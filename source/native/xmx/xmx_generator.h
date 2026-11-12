#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <cstdint>
#include <string>
#include "xmx_rules.h"

namespace xmx::generator
{
struct Pair
{
    HMODULE fg = nullptr;
    const Rvas* rvas = nullptr;
    uint32_t maxFrames = 1;
};

void Attach(const Pair& pair) noexcept;
bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) noexcept;
bool Passthrough(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) noexcept;
std::string MemoryLine() noexcept;
std::string UsedLine(ID3D12GraphicsCommandList* list) noexcept;
}
