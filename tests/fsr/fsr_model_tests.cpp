#include <bit>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fsr/fsr_model.h"

using Microsoft::WRL::ComPtr;
namespace context = xmx::context;
using xmx::generator::FrameExtras;

namespace
{
unsigned gChecks = 0, gFailures = 0;

constexpr UINT kWidth = 640, kHeight = 360;
constexpr uint32_t kMaxOutputs = 3;
constexpr float kFrameTimeMs = 16.6f;
constexpr float kJitter[2] = {0.25f, -0.125f};

struct Config
{
    const char* name;
    uint32_t flags;
    DXGI_FORMAT format;
    bool camera;
};

constexpr Config kConfigs[] = {
    {"inverted-depth-srgb-camera", context::kFlagDepthInverted, DXGI_FORMAT_R8G8B8A8_UNORM, true},
    {"jittered-scrgb-no-camera", context::kFlagMotionJittered, DXGI_FORMAT_R16G16B16A16_FLOAT, false},
};

bool Check(bool ok, const std::string& text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text.c_str());
    std::fflush(stdout);
    return ok;
}

UINT BytesPerPixel(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
}

uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) ++half;
    return static_cast<uint16_t>(sign | half);
}

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    const uint32_t mantissa = half & 0x3ffu;
    uint32_t bits;
    if (exponent == 0) bits = mantissa == 0 ? sign : sign | ((127 - 14) << 23) | (mantissa << 13);
    else bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

ComPtr<ID3D12Device> HardwareDevice()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) return nullptr;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return device;
    }
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;
};

bool MakeGpu(Gpu& gpu)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return SUCCEEDED(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&gpu.queue)))
        && SUCCEEDED(gpu.device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&gpu.allocator)))
        && SUCCEEDED(gpu.device->CreateCommandList(0, desc.Type, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)))
        && SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)));
}

bool Submit(Gpu& gpu)
{
    if (FAILED(gpu.list->Close())) return false;
    ID3D12CommandList* lists[] = {gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, lists);
    gpu.queue->Signal(gpu.fence.Get(), ++gpu.value);
    gpu.fence->SetEventOnCompletion(gpu.value, gpu.event);
    WaitForSingleObject(gpu.event, 30000);
    return gpu.fence->GetCompletedValue() >= gpu.value && SUCCEEDED(gpu.device->GetDeviceRemovedReason())
        && SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr));
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    return texture;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

UINT AlignedPitch(UINT width, DXGI_FORMAT format)
{
    const UINT bytes = width * BytesPerPixel(format);
    return (bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

D3D12_TEXTURE_COPY_LOCATION Footprint(ID3D12Resource* buffer, DXGI_FORMAT format, UINT width, UINT height)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint.Footprint.Format = format;
    location.PlacedFootprint.Footprint.Width = width;
    location.PlacedFootprint.Footprint.Height = height;
    location.PlacedFootprint.Footprint.Depth = 1;
    location.PlacedFootprint.Footprint.RowPitch = AlignedPitch(width, format);
    return location;
}

using TexelWriter = std::function<void(int x, int y, uint8_t* texel)>;

ComPtr<ID3D12Resource> UploadTexture(Gpu& gpu, DXGI_FORMAT format, UINT width, UINT height, const TexelWriter& write,
    std::vector<ComPtr<ID3D12Resource>>& keepAlive)
{
    auto texture = MakeTexture(gpu.device.Get(), width, height, format, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT pitch = AlignedPitch(width, format);
    auto upload = MakeBuffer(gpu.device.Get(), UINT64(pitch) * height, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!texture || !upload) return nullptr;
    uint8_t* bytes = nullptr;
    upload->Map(0, nullptr, reinterpret_cast<void**>(&bytes));
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) write(static_cast<int>(x), static_cast<int>(y), bytes + y * pitch + x * BytesPerPixel(format));
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const auto source = Footprint(upload.Get(), format, width, height);
    gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(gpu.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    keepAlive.push_back(upload);
    return texture;
}

struct Sample
{
    float color[3];
    float depth;
    float motion[2];
};

float Fraction(float value)
{
    return value - std::floor(value);
}

Sample SceneAt(int frame, int x, int y)
{
    const int quadX = x - 14 * frame, quadY = y - 4 * frame;
    Sample sample{};
    if (quadX >= 200 && quadX < 360 && quadY >= 100 && quadY < 220)
    {
        sample.color[0] = Fraction(static_cast<float>(quadX) / 37.f);
        sample.color[1] = Fraction(static_cast<float>(quadY) / 29.f);
        sample.color[2] = (((quadX >> 3) + (quadY >> 3)) & 1) ? 0.9f : 0.1f;
        sample.depth = 0.8f;
        sample.motion[0] = -14.f;
        sample.motion[1] = -4.f;
        return sample;
    }
    const int u = x - 6 * frame;
    sample.color[0] = Fraction(static_cast<float>(u) / 64.f);
    sample.color[1] = Fraction(static_cast<float>(y) / 48.f);
    sample.color[2] = (((u >> 4) + (y >> 4)) & 1) ? 0.8f : 0.2f;
    sample.depth = 0.2f;
    sample.motion[0] = -6.f;
    return sample;
}

void PackColor(DXGI_FORMAT format, const Sample& sample, uint8_t* texel)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        auto* halves = reinterpret_cast<uint16_t*>(texel);
        for (int channel = 0; channel < 3; ++channel) halves[channel] = FloatToHalf(sample.color[channel]);
        halves[3] = FloatToHalf(1.f);
        return;
    }
    for (int channel = 0; channel < 3; ++channel)
        texel[channel] = static_cast<uint8_t>(std::lround(sample.color[channel] * 255.f));
    texel[3] = 255;
}

struct Inputs
{
    Config config;
    ComPtr<ID3D12Resource> color[2], depth[2], motion[2];
    std::vector<ComPtr<ID3D12Resource>> uploads;
};

bool MakeInputs(Gpu& gpu, const Config& config, Inputs& inputs)
{
    inputs.config = config;
    const bool inverted = (config.flags & context::kFlagDepthInverted) != 0;
    for (int frame = 0; frame < 2; ++frame)
    {
        inputs.color[frame] = UploadTexture(gpu, config.format, kWidth, kHeight,
            [&](int x, int y, uint8_t* texel) { PackColor(config.format, SceneAt(frame, x, y), texel); }, inputs.uploads);
        inputs.depth[frame] = UploadTexture(gpu, DXGI_FORMAT_R32_FLOAT, kWidth, kHeight,
            [&](int x, int y, uint8_t* texel) {
                const float depth = SceneAt(frame, x, y).depth;
                const float value = inverted ? depth : 1.f - depth;
                std::memcpy(texel, &value, sizeof(value));
            }, inputs.uploads);
        inputs.motion[frame] = UploadTexture(gpu, DXGI_FORMAT_R16G16_FLOAT, kWidth, kHeight,
            [&](int x, int y, uint8_t* texel) {
                const Sample sample = SceneAt(frame, x, y);
                auto* halves = reinterpret_cast<uint16_t*>(texel);
                halves[0] = FloatToHalf(sample.motion[0]);
                halves[1] = FloatToHalf(sample.motion[1]);
            }, inputs.uploads);
        if (!inputs.color[frame] || !inputs.depth[frame] || !inputs.motion[frame]) return false;
    }
    const bool submitted = Submit(gpu);
    inputs.uploads.clear();
    return submitted;
}

FrameExtras ExtrasFor(const Config& config)
{
    FrameExtras extras;
    extras.frameTimeMs = kFrameTimeMs;
    if (config.camera)
    {
        extras.cameraNear = 0.1f;
        extras.cameraFar = 1000.f;
        extras.cameraFovY = 1.f;
    }
    return extras;
}

std::vector<ComPtr<ID3D12Resource>> MakeOutputs(Gpu& gpu, const Config& config, uint32_t count)
{
    std::vector<ComPtr<ID3D12Resource>> outputs;
    for (uint32_t index = 0; index < count; ++index)
        outputs.push_back(MakeTexture(gpu.device.Get(), kWidth, kHeight, config.format,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    return outputs;
}

std::vector<uint8_t> ReadBack(Gpu& gpu, const Config& config, ID3D12Resource* texture)
{
    const UINT pitch = AlignedPitch(kWidth, config.format);
    auto readback = MakeBuffer(gpu.device.Get(), UINT64(pitch) * kHeight, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> packed;
    if (!readback) return packed;
    Transition(gpu.list.Get(), texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const auto destination = Footprint(readback.Get(), config.format, kWidth, kHeight);
    gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(gpu.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!Submit(gpu)) return packed;
    const UINT row = kWidth * BytesPerPixel(config.format);
    uint8_t* bytes = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&bytes));
    packed.resize(size_t(row) * kHeight);
    for (UINT y = 0; y < kHeight; ++y) std::memcpy(packed.data() + size_t(y) * row, bytes + size_t(y) * pitch, row);
    readback->Unmap(0, nullptr);
    return packed;
}

bool IsBlank(const std::vector<uint8_t>& pixels)
{
    return std::all_of(pixels.begin(), pixels.end(), [](uint8_t byte) { return byte == 0; });
}

struct Difference
{
    double maxChannel = 0.0;
    size_t channels = 0;
};

Difference Measure(const Config& config, const std::vector<uint8_t>& left, const std::vector<uint8_t>& right)
{
    Difference difference;
    const bool half = config.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const size_t step = half ? 2 : 1;
    for (size_t offset = 0; offset + step <= left.size(); offset += step)
    {
        if (std::memcmp(left.data() + offset, right.data() + offset, step) == 0) continue;
        ++difference.channels;
        double a, b;
        if (half)
        {
            uint16_t x, y;
            std::memcpy(&x, left.data() + offset, 2);
            std::memcpy(&y, right.data() + offset, 2);
            a = HalfToFloat(x);
            b = HalfToFloat(y);
        }
        else
        {
            a = left[offset] / 255.0;
            b = right[offset] / 255.0;
        }
        difference.maxChannel = std::max(difference.maxChannel, std::fabs(a - b));
    }
    return difference;
}

context::Input InputOf(ID3D12Resource* resource)
{
    return {resource, 0, 0, kWidth, kHeight};
}

context::ExecuteParams ExecuteOf(const Inputs& inputs, int frame, ID3D12Resource** outputs, uint32_t count,
    uint32_t firstFrame)
{
    static const uint32_t bases[kMaxOutputs * 2]{};
    context::ExecuteParams execute{};
    for (auto* data : {&execute.previous, &execute.next})
    {
        const int slot = data == &execute.next ? frame : frame ^ 1;
        data->backbuffer = InputOf(inputs.color[slot].Get());
        data->depth = InputOf(inputs.depth[slot].Get());
        data->motion = InputOf(inputs.motion[slot].Get());
        data->resolution[0] = static_cast<float>(kWidth);
        data->resolution[1] = static_cast<float>(kHeight);
        data->jitter[0] = kJitter[0];
        data->jitter[1] = kJitter[1];
        data->motionScale[0] = data->motionScale[1] = 1.f;
    }
    execute.outputs = outputs;
    execute.outputBases = bases;
    execute.outputCount = static_cast<uint8_t>(count);
    execute.firstFrame = firstFrame;
    execute.frameCount = 1;
    return execute;
}

struct Adapter
{
    xmx::generator::Pair pair;
    void* handle = nullptr;

    bool Open(Gpu& gpu, const Config& config)
    {
        std::string failure;
        if (!fsr::Model().create(pair, gpu.device.Get(), handle, failure) || !handle)
        {
            std::printf("create failed: %s\n", failure.c_str());
            return false;
        }
        context::InitParams init{};
        init.width = kWidth;
        init.height = kHeight;
        init.format = config.format;
        init.flags = config.flags;
        init.maxFrames = kMaxOutputs;
        init.creationMask = 1;
        init.visibleMask = 1;
        const int result = fsr::Model().initialize(pair, handle, init);
        if (result != 0) std::printf("initialize returned %d\n", result);
        return result == 0;
    }

    int Execute(Gpu& gpu, const Inputs& inputs, context::ExecuteParams& execute)
    {
        return fsr::Model().execute(pair, handle, gpu.list.Get(), execute, ExtrasFor(inputs.config));
    }
};

std::vector<uint32_t> Indices(uint32_t count)
{
    std::vector<uint32_t> indices(count);
    for (uint32_t index = 0; index < count; ++index) indices[index] = index;
    return indices;
}

bool RunPair(Gpu& gpu, Adapter& adapter, const Inputs& inputs, int frame,
    std::vector<ComPtr<ID3D12Resource>>& outputs, const std::vector<uint32_t>& indices)
{
    ID3D12Resource* raw[kMaxOutputs]{};
    for (size_t index = 0; index < outputs.size(); ++index) raw[index] = outputs[index].Get();
    bool ok = true;
    for (uint32_t index : indices)
    {
        auto execute = ExecuteOf(inputs, frame, raw, static_cast<uint32_t>(outputs.size()), index);
        const int result = adapter.Execute(gpu, inputs, execute);
        if (result != 0) std::printf("execute of index %u returned %d\n", index, result);
        ok = ok && result == 0;
    }
    return Submit(gpu) && ok;
}

struct Direct
{
    FfxInterface shared{};
    FfxInterface effects{};
    std::vector<uint64_t> sharedScratch, effectScratch;
    FfxUInt32 sharedContext = 0;
    FfxOpticalflowContext opticalFlow{};
    FfxFrameInterpolationContext interpolation{};
    FfxResourceInternal flowVector{}, flowSceneChange{}, dilatedDepth{}, dilatedMotionVectors{}, reconstructedDepth{};
    uint64_t frameID = 0;
    Config config{};

    bool Open(Gpu& gpu, const Config& configuration)
    {
        config = configuration;
        const size_t sharedSize = ffxGetScratchMemorySizeDX12(1);
        const size_t effectSize = ffxGetScratchMemorySizeDX12(2);
        sharedScratch.assign(sharedSize / 8 + 1, 0);
        effectScratch.assign(effectSize / 8 + 1, 0);
        if (ffxGetInterfaceDX12(&shared, ffxGetDeviceDX12(gpu.device.Get()), sharedScratch.data(), sharedSize, 1) != FFX_OK
            || ffxGetInterfaceDX12(&effects, ffxGetDeviceDX12(gpu.device.Get()), effectScratch.data(), effectSize, 2) != FFX_OK
            || shared.fpCreateBackendContext(&shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &sharedContext) != FFX_OK)
            return false;
        FfxOpticalflowContextDescription flow{};
        flow.backendInterface = effects;
        flow.resolution = {kWidth, kHeight};
        FfxOpticalflowSharedResourceDescriptions flowResources{};
        if (ffxOpticalflowContextCreate(&opticalFlow, &flow) != FFX_OK
            || ffxOpticalflowGetSharedResourceDescriptions(&opticalFlow, &flowResources) != FFX_OK
            || shared.fpCreateResource(&shared, &flowResources.opticalFlowVector, sharedContext, &flowVector) != FFX_OK
            || shared.fpCreateResource(&shared, &flowResources.opticalFlowSCD, sharedContext, &flowSceneChange) != FFX_OK)
            return false;
        FfxFrameInterpolationContextDescription description{};
        description.backendInterface = effects;
        if (config.flags & context::kFlagDepthInverted) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
        if (config.flags & context::kFlagMotionJittered) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS;
        if (!config.camera) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
        description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        description.maxRenderSize = {kWidth, kHeight};
        description.displaySize = {kWidth, kHeight};
        description.backBufferFormat = ffxGetSurfaceFormatDX12(config.format);
        description.previousInterpolationSourceFormat = description.backBufferFormat;
        FfxFrameInterpolationSharedResourceDescriptions resources{};
        return ffxFrameInterpolationContextCreate(&interpolation, &description) == FFX_OK
            && ffxFrameInterpolationGetSharedResourceDescriptions(&interpolation, &resources) == FFX_OK
            && shared.fpCreateResource(&shared, &resources.dilatedDepth, sharedContext, &dilatedDepth) == FFX_OK
            && shared.fpCreateResource(&shared, &resources.dilatedMotionVectors, sharedContext, &dilatedMotionVectors) == FFX_OK
            && shared.fpCreateResource(&shared, &resources.reconstructedPrevNearestDepth, sharedContext,
                   &reconstructedDepth) == FFX_OK;
    }

    FfxResource Shared(FfxResourceInternal resource)
    {
        return shared.fpGetResource(&shared, resource);
    }

    static FfxResource Wrap(ID3D12Resource* resource, const wchar_t* name, FfxResourceStates state, FfxResourceUsage usage)
    {
        return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource, usage), name, state);
    }

    bool RunPair(Gpu& gpu, const Inputs& inputs, int frame, ID3D12Resource* output)
    {
        const bool reset = frameID == 0;
        ++frameID;
        const FrameExtras extras = ExtrasFor(config);
        const FrameExtras camera = config.camera ? extras : FrameExtras{0.1f, FLT_MAX, 1.0472f, extras.frameTimeMs};
        const bool half = config.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const float maxLuminance = half ? 12.5f : 1.f;
        const FfxCommandList commandList = ffxGetCommandListDX12(gpu.list.Get());
        const FfxResource color = Wrap(inputs.color[frame].Get(), L"Color", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);

        FfxFrameInterpolationPrepareDescription prepare{};
        prepare.commandList = commandList;
        prepare.renderSize = {kWidth, kHeight};
        prepare.jitterOffset = {kJitter[0], kJitter[1]};
        prepare.motionVectorScale = {1.f, 1.f};
        prepare.frameTimeDelta = extras.frameTimeMs;
        prepare.cameraNear = camera.cameraNear;
        prepare.cameraFar = camera.cameraFar;
        prepare.viewSpaceToMetersFactor = 1.f;
        prepare.cameraFovAngleVertical = camera.cameraFovY;
        prepare.depth = Wrap(inputs.depth[frame].Get(), L"Depth", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
        prepare.motionVectors = Wrap(inputs.motion[frame].Get(), L"MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ,
            FFX_RESOURCE_USAGE_READ_ONLY);
        prepare.frameID = frameID;
        prepare.dilatedDepth = Shared(dilatedDepth);
        prepare.dilatedMotionVectors = Shared(dilatedMotionVectors);
        prepare.reconstructedPrevDepth = Shared(reconstructedDepth);
        if (ffxFrameInterpolationPrepare(&interpolation, &prepare) != FFX_OK) return false;

        FfxOpticalflowDispatchDescription flow{};
        flow.commandList = commandList;
        flow.color = color;
        flow.reset = reset;
        const FfxBackbufferTransferFunction transfer =
            half ? FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB : FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        flow.backbufferTransferFunction = transfer;
        flow.minMaxLuminance = {0.f, maxLuminance};
        flow.opticalFlowVector = Shared(flowVector);
        flow.opticalFlowSCD = Shared(flowSceneChange);
        if (ffxOpticalflowContextDispatch(&opticalFlow, &flow) != FFX_OK) return false;

        FfxFrameInterpolationDispatchDescription dispatch{};
        dispatch.commandList = commandList;
        dispatch.displaySize = {kWidth, kHeight};
        dispatch.renderSize = {kWidth, kHeight};
        dispatch.currentBackBuffer = color;
        dispatch.output = Wrap(output, L"Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
        dispatch.interpolationRect = {0, 0, static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight)};
        dispatch.opticalFlowVector = Shared(flowVector);
        dispatch.opticalFlowSceneChangeDetection = Shared(flowSceneChange);
        dispatch.opticalFlowBlockSize = 8;
        dispatch.opticalFlowScale = {1.f / static_cast<float>(kWidth), 1.f / static_cast<float>(kHeight)};
        dispatch.cameraNear = camera.cameraNear;
        dispatch.cameraFar = camera.cameraFar;
        dispatch.cameraFovAngleVertical = camera.cameraFovY;
        dispatch.viewSpaceToMetersFactor = 1.f;
        dispatch.frameTimeDelta = extras.frameTimeMs;
        dispatch.reset = reset;
        dispatch.backBufferTransferFunction = transfer;
        dispatch.minMaxLuminance[0] = 0.f;
        dispatch.minMaxLuminance[1] = maxLuminance;
        dispatch.frameID = frameID;
        dispatch.dilatedDepth = Shared(dilatedDepth);
        dispatch.dilatedMotionVectors = Shared(dilatedMotionVectors);
        dispatch.reconstructedPrevDepth = Shared(reconstructedDepth);
        return ffxFrameInterpolationDispatch(&interpolation, &dispatch) == FFX_OK && Submit(gpu);
    }
};

std::string Label(const char* test, const Config& config, const char* what)
{
    return std::string(test) + " [" + config.name + "]: " + what;
}

bool TwoXOutput(Gpu& gpu, const Inputs& inputs, std::vector<uint8_t>& pixels)
{
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 1);
    if (!adapter.Open(gpu, inputs.config) || !outputs[0]) return false;
    if (!RunPair(gpu, adapter, inputs, 0, outputs, Indices(1)) || !RunPair(gpu, adapter, inputs, 1, outputs, Indices(1)))
        return false;
    pixels = ReadBack(gpu, inputs.config, outputs[0].Get());
    return !pixels.empty();
}

void TwoXMatchesOriginal(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "two_x_matches_original";
    std::vector<uint8_t> adapterPixels, directPixels;
    auto direct = std::make_unique<Direct>();
    auto output = MakeOutputs(gpu, inputs.config, 1);
    const bool ran = TwoXOutput(gpu, inputs, adapterPixels) && direct->Open(gpu, inputs.config) && output[0]
        && direct->RunPair(gpu, inputs, 0, output[0].Get()) && direct->RunPair(gpu, inputs, 1, output[0].Get());
    if (!Check(ran, Label(test, inputs.config, "the adapter and the original FFX path both ran"))) return;
    directPixels = ReadBack(gpu, inputs.config, output[0].Get());
    Check(!IsBlank(adapterPixels), Label(test, inputs.config, "the generated frame is not blank"));
    const auto difference = Measure(inputs.config, adapterPixels, directPixels);
    std::printf("%s: differing channels %zu, max channel difference %g\n", test, difference.channels,
        difference.maxChannel);
    Check(adapterPixels == directPixels, Label(test, inputs.config, "the adapter's 2x output is bit-identical to the original FFX path"));
}

void FourXMiddleMatchesTwoX(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "four_x_middle_matches_two_x";
    std::vector<uint8_t> twoX;
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 3);
    const bool ran = TwoXOutput(gpu, inputs, twoX) && adapter.Open(gpu, inputs.config) && outputs[0] && outputs[1] && outputs[2]
        && RunPair(gpu, adapter, inputs, 0, outputs, Indices(3)) && RunPair(gpu, adapter, inputs, 1, outputs, Indices(3));
    if (!Check(ran, Label(test, inputs.config, "the 2x and 4x runs both ran"))) return;
    const auto middle = ReadBack(gpu, inputs.config, outputs[1].Get());
    const auto difference = Measure(inputs.config, twoX, middle);
    std::printf("FOUR_X_MIDDLE %s differing channels %zu, max channel difference %g\n", inputs.config.name,
        difference.channels, difference.maxChannel);
    Check(twoX == middle, Label(test, inputs.config, "the 4x middle frame (tau 0.5) is bit-identical to the 2x frame"));
}

void FourXInstantsDiffer(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "four_x_instants_differ";
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 3);
    const bool ran = adapter.Open(gpu, inputs.config) && outputs[0] && outputs[1] && outputs[2]
        && RunPair(gpu, adapter, inputs, 0, outputs, Indices(3)) && RunPair(gpu, adapter, inputs, 1, outputs, Indices(3));
    if (!Check(ran, Label(test, inputs.config, "the 4x pairs ran"))) return;
    const auto first = ReadBack(gpu, inputs.config, outputs[0].Get());
    const auto middle = ReadBack(gpu, inputs.config, outputs[1].Get());
    const auto last = ReadBack(gpu, inputs.config, outputs[2].Get());
    Check(first != middle, Label(test, inputs.config, "output 0 differs from output 1"));
    Check(middle != last, Label(test, inputs.config, "output 1 differs from output 2"));
    Check(first != last, Label(test, inputs.config, "output 0 differs from output 2"));
}

void SkippedLastResets(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "skipped_last_resets";
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 3);
    if (!Check(adapter.Open(gpu, inputs.config) && outputs[2], Label(test, inputs.config, "the context opens"))) return;
    const auto start = fsr::ReadStats();
    bool ran = RunPair(gpu, adapter, inputs, 0, outputs, {0, 1});
    const auto afterPartial = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 1, outputs, Indices(3)) && ran;
    const auto afterFull = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 0, outputs, Indices(3)) && ran;
    const auto afterSecondFull = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 1, outputs, Indices(3)) && ran;
    const auto afterThirdFull = fsr::ReadStats();
    Check(ran, Label(test, inputs.config, "every pair ran"));
    Check(afterPartial.resets == start.resets + 1, Label(test, inputs.config, "the context's first pair resets"));
    Check(afterFull.resets == afterPartial.resets + 1,
        Label(test, inputs.config, "the pair after one that skipped its last frame resets"));
    Check(afterSecondFull.resets == afterFull.resets,
        Label(test, inputs.config, "a full pair after a full pair does not reset"));
    Check(afterThirdFull.resets == afterSecondFull.resets,
        Label(test, inputs.config, "a second full pair in a row does not reset"));
    Check(afterThirdFull.pairs == start.pairs + 4, Label(test, inputs.config, "four pairs were counted"));
}

void SkippedPairResets(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "skipped_pair_resets";
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 3);
    if (!Check(adapter.Open(gpu, inputs.config) && outputs[2], Label(test, inputs.config, "the context opens"))) return;
    const auto start = fsr::ReadStats();
    bool ran = RunPair(gpu, adapter, inputs, 0, outputs, Indices(3));
    const auto afterFirst = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 1, outputs, Indices(3)) && ran;
    const auto afterSecond = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 1, outputs, Indices(3)) && ran;
    const auto afterRepeat = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 0, outputs, Indices(3)) && ran;
    const auto afterAlternate = fsr::ReadStats();
    ran = RunPair(gpu, adapter, inputs, 1, outputs, Indices(3)) && ran;
    const auto afterLast = fsr::ReadStats();
    Check(ran, Label(test, inputs.config, "every pair ran"));
    Check(afterFirst.resets == start.resets + 1, Label(test, inputs.config, "the context's first pair resets"));
    Check(afterSecond.resets == afterFirst.resets, Label(test, inputs.config, "a full pair on the other color does not reset"));
    Check(afterRepeat.resets == afterSecond.resets + 1,
        Label(test, inputs.config, "a pair on the same color as the last pair resets, as after a skipped pair"));
    Check(afterAlternate.resets == afterRepeat.resets, Label(test, inputs.config, "a full pair on the other color after the reset does not reset"));
    Check(afterLast.resets == afterAlternate.resets, Label(test, inputs.config, "the alternating full pairs never reset"));
    Check(afterLast.pairs == start.pairs + 5, Label(test, inputs.config, "five pairs were counted"));
}

void OffsetRefused(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "offset_refused";
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 1);
    if (!Check(adapter.Open(gpu, inputs.config) && outputs[0], Label(test, inputs.config, "the context opens"))) return;
    ID3D12Resource* raw[kMaxOutputs]{outputs[0].Get()};
    const auto start = fsr::ReadStats();
    auto depthOffset = ExecuteOf(inputs, 0, raw, 1, 0);
    depthOffset.next.depth.baseX = 8;
    const int depthResult = adapter.Execute(gpu, inputs, depthOffset);
    const auto afterDepth = fsr::ReadStats();
    auto motionOffset = ExecuteOf(inputs, 0, raw, 1, 0);
    motionOffset.next.motion.baseY = 4;
    const int motionResult = adapter.Execute(gpu, inputs, motionOffset);
    const auto afterMotion = fsr::ReadStats();
    Check(depthResult == 1, Label(test, inputs.config, "an offset depth input returns 1"));
    Check(afterDepth.refusals == start.refusals + 1, Label(test, inputs.config, "the depth refusal is counted"));
    Check(afterDepth.dispatches == start.dispatches, Label(test, inputs.config, "the depth refusal dispatches nothing"));
    Check(motionResult == 1, Label(test, inputs.config, "an offset motion input returns 1"));
    Check(afterMotion.refusals == afterDepth.refusals + 1, Label(test, inputs.config, "the motion refusal is counted"));
    Check(afterMotion.dispatches == start.dispatches, Label(test, inputs.config, "the motion refusal dispatches nothing"));
    auto clean = ExecuteOf(inputs, 0, raw, 1, 0);
    Check(adapter.Execute(gpu, inputs, clean) == 0 && Submit(gpu), Label(test, inputs.config, "the same inputs without an offset generate"));
    Check(fsr::ReadStats().dispatches == start.dispatches + 1, Label(test, inputs.config, "the clean frame dispatches once"));
}

void LaterIndexFirst(Gpu& gpu, const Inputs& inputs)
{
    const char* test = "later_index_first";
    Adapter adapter;
    auto outputs = MakeOutputs(gpu, inputs.config, 3);
    if (!Check(adapter.Open(gpu, inputs.config) && outputs[2], Label(test, inputs.config, "the context opens"))) return;
    ID3D12Resource* raw[kMaxOutputs]{outputs[0].Get(), outputs[1].Get(), outputs[2].Get()};
    const auto start = fsr::ReadStats();
    auto later = ExecuteOf(inputs, 0, raw, 3, 1);
    Check(adapter.Execute(gpu, inputs, later) == 1, Label(test, inputs.config, "index 1 on a fresh context returns 1"));
    Check(fsr::ReadStats().dispatches == start.dispatches, Label(test, inputs.config, "index 1 on a fresh context dispatches nothing"));
    Check(RunPair(gpu, adapter, inputs, 0, outputs, Indices(3)), Label(test, inputs.config, "a full pair afterwards generates"));
    Check(fsr::ReadStats().dispatches == start.dispatches + 3, Label(test, inputs.config, "the full pair dispatches three times"));
}

struct TestCase
{
    const char* name;
    void (*run)(Gpu&, const Inputs&);
};

constexpr TestCase kTests[] = {
    {"two_x_matches_original", TwoXMatchesOriginal},
    {"four_x_middle_matches_two_x", FourXMiddleMatchesTwoX},
    {"four_x_instants_differ", FourXInstantsDiffer},
    {"skipped_last_resets", SkippedLastResets},
    {"skipped_pair_resets", SkippedPairResets},
    {"offset_refused", OffsetRefused},
    {"later_index_first", LaterIndexFirst},
};

int RunTest(const TestCase& test)
{
    Gpu gpu;
    gpu.device = HardwareDevice();
    if (!gpu.device || !MakeGpu(gpu))
    {
        std::printf("SETUP no D3D12 hardware device at feature level 12_0\n");
        return 2;
    }
    for (const Config& config : kConfigs)
    {
        Inputs inputs;
        if (!Check(MakeInputs(gpu, config, inputs), std::string("the inputs upload [") + config.name + "]")) return 1;
        test.run(gpu, inputs);
    }
    const auto stats = fsr::ReadStats();
    std::printf("FSR_MODEL_TESTS %s checks=%u failures=%u pairs=%llu dispatches=%llu resets=%llu refusals=%llu\n", test.name,
        gChecks, gFailures, static_cast<unsigned long long>(stats.pairs), static_cast<unsigned long long>(stats.dispatches),
        static_cast<unsigned long long>(stats.resets), static_cast<unsigned long long>(stats.refusals));
    return gFailures ? 1 : 0;
}
}

namespace xmx
{
void Log(const std::string& text) noexcept
{
    std::printf("log: %s\n", text.c_str());
}
}

int wmain(int argc, wchar_t** argv)
{
    wchar_t gate[8]{};
    if (GetEnvironmentVariableW(L"MFG_TEST_GPU", gate, 8) == 0 || gate[0] != L'1')
    {
        std::printf("SETUP creates a GPU device: set MFG_TEST_GPU=1 with the game closed\n");
        return 2;
    }
    if (argc < 2) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring selected = argv[1];
    for (const TestCase& test : kTests)
    {
        const std::string name = test.name;
        if (selected == std::wstring(name.begin(), name.end())) return RunTest(test);
    }
    std::printf("unknown test\n");
    return 1;
}
