#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include "xmx_rules.h"

namespace xmx::context
{
#pragma pack(push, 8)
struct InitParams
{
    uint32_t width, height, format, flags, maxFrames, creationMask, visibleMask, reserved;
    void* tempBufferHeap;
    uint64_t bufferOffset;
    void* tempTextureHeap;
    uint64_t textureOffset;
    void* pipelineLibrary;
    uint32_t uiMode, reserved2;
};

struct Input
{
    ID3D12Resource* resource;
    uint32_t baseX, baseY, width, height;
};

struct FrameData
{
    Input backbuffer, depth, motion, ui, hudless;
    float view[16], projection[16], resolution[2], jitter[2], motionScale[2];
    uint32_t reset;
    float renderTimeMs;
};

struct ExecuteParams
{
    FrameData previous, next;
    ID3D12Resource* const* outputs;
    const uint32_t* outputBases;
    uint8_t outputCount;
    uint8_t reserved[3];
    uint32_t networkScales;
    void* descriptorHeap;
    uint32_t heapOffset, uiComposition;
    uint32_t firstFrame, frameCount;
};
#pragma pack(pop)

static_assert(sizeof(InitParams) == 80 && sizeof(Input) == 24 && sizeof(FrameData) == 0x118);
static_assert(offsetof(FrameData, view) == 0x78 && offsetof(FrameData, reset) == 0x110);
static_assert(offsetof(ExecuteParams, next) == 0x118 && offsetof(ExecuteParams, outputs) == 0x230);
static_assert(offsetof(ExecuteParams, outputCount) == 0x240 && offsetof(ExecuteParams, descriptorHeap) == 0x248);
static_assert(offsetof(ExecuteParams, firstFrame) == 0x258 && sizeof(ExecuteParams) == 0x260);

constexpr uint32_t kFlagDepthInverted = 1u << 0;
constexpr uint32_t kFlagMotionJittered = 1u << 4;
constexpr int kCrashed = -100000;

int Guarded(void* function, void* handle, void* first, void* second) noexcept;
bool Create(HMODULE fg, const Rvas& rvas, ID3D12Device* device, void*& handle, std::string& failure) noexcept;
int Initialize(HMODULE fg, const Rvas& rvas, void* handle, InitParams& params) noexcept;
int Execute(HMODULE fg, const Rvas& rvas, void* handle, ID3D12GraphicsCommandList* list,
    ExecuteParams& params) noexcept;
}
