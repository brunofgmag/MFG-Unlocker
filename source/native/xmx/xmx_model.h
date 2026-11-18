#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include "xmx_context.h"
#include "xmx_rules.h"

namespace xmx::generator
{
struct Model;

struct Pair
{
    HMODULE fg = nullptr;
    const Rvas* rvas = nullptr;
    uint32_t maxFrames = 1;
    const Model* fsr = nullptr;
};

struct FrameExtras
{
    float cameraNear = 0.f;
    float cameraFar = 0.f;
    float cameraFovY = 0.f;
    float frameTimeMs = 0.f;
};

struct Model
{
    const char* name;
    uint32_t maxFrames;
    bool (*create)(const Pair& pair, ID3D12Device* device, void*& handle, std::string& failure) noexcept;
    int (*initialize)(const Pair& pair, void* handle, context::InitParams& init) noexcept;
    int (*execute)(const Pair& pair, void* handle, ID3D12GraphicsCommandList* list, context::ExecuteParams& execute,
        const FrameExtras& extras) noexcept;
};
}
