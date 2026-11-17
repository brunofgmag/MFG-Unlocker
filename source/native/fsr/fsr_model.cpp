#include "fsr/fsr_model.h"

#include <bit>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "xmx/xmx_status.h"

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainDX12(FfxFrameGenerationConfig const*)
{
    return FFX_OK;
}

namespace fsr
{
namespace
{
using xmx::generator::FrameExtras;
using xmx::generator::Pair;
namespace context = xmx::context;

constexpr float kDefaultNear = 0.1f;
constexpr float kDefaultFovY = 1.0472f;
constexpr float kDefaultFrameTimeMs = 33.3f;
constexpr int kOpticalFlowBlockSize = 8;

enum Failure : int
{
    kBadShape = -1,
    kBackend = -2,
    kOpticalFlowContext = -3,
    kInterpolationContext = -4,
    kPrepare = -5,
    kOpticalFlowDispatch = -6,
    kInterpolationDispatch = -7,
    kException = -8,
};

std::atomic<uint64_t> gPairs{0}, gDispatches{0}, gResets{0}, gRefusals{0};

struct Transfer
{
    FfxBackbufferTransferFunction function;
    float minLuminance, maxLuminance;
    const char* name;
};

struct Camera
{
    float nearPlane = kDefaultNear;
    float farPlane = FLT_MAX;
    float fovY = kDefaultFovY;
};

struct FreeDeleter
{
    void operator()(void* memory) const noexcept
    {
        std::free(memory);
    }
};

Transfer TransferFor(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return {FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB, 0.f, 12.5f, "scRGB"};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return {FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ, 0.f, 1000.f, "PQ"};
    default: return {FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB, 0.f, 1.f, "sRGB"};
    }
}

bool Usable(float value) noexcept
{
    return std::isfinite(value) && value > 0.f;
}

bool HasOffset(const context::Input& input) noexcept
{
    return input.baseX != 0 || input.baseY != 0;
}

float FrameTime(const FrameExtras& extras) noexcept
{
    return Usable(extras.frameTimeMs) ? extras.frameTimeMs : kDefaultFrameTimeMs;
}

float InterpolationInstant(uint32_t index, uint32_t count) noexcept
{
    return static_cast<float>(index + 1) / static_cast<float>(count + 1);
}

FfxResource Wrap(ID3D12Resource* resource, const wchar_t* name, FfxResourceStates state, FfxResourceUsage usage)
{
    return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource, usage), name, state);
}

FfxResource Input(ID3D12Resource* resource, const wchar_t* name)
{
    return Wrap(resource, name, FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
}

FfxResource Output(ID3D12Resource* resource, const wchar_t* name)
{
    return Wrap(resource, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
}

struct Instance
{
    FfxInterface shared{};
    FfxInterface effects{};
    std::unique_ptr<void, FreeDeleter> sharedScratch, effectScratch;
    FfxUInt32 sharedContext = 0;
    FfxOpticalflowContext opticalFlow{};
    FfxFrameInterpolationContext interpolation{};
    FfxResourceInternal flowVector{}, flowSceneChange{}, dilatedDepth{}, dilatedMotionVectors{}, reconstructedDepth{};

    uint32_t width = 0, height = 0, renderWidth = 0, renderHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool depthInverted = false, jittered = false;
    Transfer transfer{};
    Camera camera;

    bool interpolationReady = false, interpolationFailed = false;
    bool historyValid = false, prepared = false, pairReset = false;
    const ID3D12Resource* lastColor = nullptr;
    uint64_t frameID = 0;
    std::set<std::string> logged;

    void LogOnce(const std::string& text);
    bool Succeeded(FfxErrorCode code, const char* stage);
    FfxResource Shared(FfxResourceInternal resource);
    bool CreateBackends(ID3D12Device* device);
    bool CreateOpticalFlow();
    bool CreateInterpolation(bool infiniteDepth);
    bool EnsureInterpolation(const context::FrameData& next);
    int Initialize(const context::InitParams& init);
    void ResolveCamera(const FrameExtras& extras);
    const char* RefusalReason(const context::ExecuteParams& execute) const;
    bool TakeHistory(const ID3D12Resource* color);
    int BeginPair(ID3D12GraphicsCommandList* list, const context::FrameData& next, const FrameExtras& extras,
        bool reset);
    bool Prepare(ID3D12GraphicsCommandList* list, const context::FrameData& next, const FrameExtras& extras);
    bool DispatchOpticalFlow(ID3D12GraphicsCommandList* list, const context::FrameData& next);
    int GenerateFrame(ID3D12GraphicsCommandList* list, const context::ExecuteParams& execute,
        const FrameExtras& extras);
    int Run(ID3D12GraphicsCommandList* list, const context::ExecuteParams& execute, const FrameExtras& extras);
};

void Instance::LogOnce(const std::string& text)
{
    if (logged.insert(text).second) xmx::Log(text);
}

bool Instance::Succeeded(FfxErrorCode code, const char* stage)
{
    if (code == FFX_OK) return true;
    char text[96];
    std::snprintf(text, sizeof(text), "FSR: %s failed with FFX error 0x%08X", stage, static_cast<unsigned>(code));
    LogOnce(text);
    return false;
}

FfxResource Instance::Shared(FfxResourceInternal resource)
{
    return shared.fpGetResource(&shared, resource);
}

bool Instance::CreateBackends(ID3D12Device* device)
{
    const size_t sharedSize = ffxGetScratchMemorySizeDX12(1);
    const size_t effectSize = ffxGetScratchMemorySizeDX12(2);
    sharedScratch.reset(std::calloc(1, sharedSize));
    effectScratch.reset(std::calloc(1, effectSize));
    if (!sharedScratch || !effectScratch) return false;
    return Succeeded(ffxGetInterfaceDX12(&shared, ffxGetDeviceDX12(device), sharedScratch.get(), sharedSize, 1),
               "shared backend interface")
        && Succeeded(ffxGetInterfaceDX12(&effects, ffxGetDeviceDX12(device), effectScratch.get(), effectSize, 2),
               "effect backend interface")
        && Succeeded(shared.fpCreateBackendContext(&shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &sharedContext),
               "shared backend context");
}

bool Instance::CreateOpticalFlow()
{
    FfxOpticalflowContextDescription description{};
    description.backendInterface = effects;
    description.resolution = {width, height};
    FfxOpticalflowSharedResourceDescriptions resources{};
    return Succeeded(ffxOpticalflowContextCreate(&opticalFlow, &description), "optical flow context")
        && Succeeded(ffxOpticalflowGetSharedResourceDescriptions(&opticalFlow, &resources),
               "optical flow shared descriptions")
        && Succeeded(shared.fpCreateResource(&shared, &resources.opticalFlowVector, sharedContext, &flowVector),
               "optical flow vector")
        && Succeeded(shared.fpCreateResource(&shared, &resources.opticalFlowSCD, sharedContext, &flowSceneChange),
               "optical flow scene change");
}

bool Instance::CreateInterpolation(bool infiniteDepth)
{
    FfxFrameInterpolationContextDescription description{};
    description.backendInterface = effects;
    if (depthInverted) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
    if (jittered) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS;
    if (infiniteDepth) description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE;
    if (renderWidth == width && renderHeight == height)
        description.flags |= FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    description.maxRenderSize = {width, height};
    description.displaySize = {width, height};
    description.backBufferFormat = ffxGetSurfaceFormatDX12(format);
    description.previousInterpolationSourceFormat = description.backBufferFormat;
    FfxFrameInterpolationSharedResourceDescriptions resources{};
    return Succeeded(ffxFrameInterpolationContextCreate(&interpolation, &description), "frame interpolation context")
        && Succeeded(ffxFrameInterpolationGetSharedResourceDescriptions(&interpolation, &resources),
               "frame interpolation shared descriptions")
        && Succeeded(shared.fpCreateResource(&shared, &resources.dilatedDepth, sharedContext, &dilatedDepth),
               "dilated depth")
        && Succeeded(shared.fpCreateResource(&shared, &resources.dilatedMotionVectors, sharedContext,
                         &dilatedMotionVectors),
               "dilated motion vectors")
        && Succeeded(shared.fpCreateResource(&shared, &resources.reconstructedPrevNearestDepth, sharedContext,
                         &reconstructedDepth),
               "reconstructed previous depth");
}

int Instance::Initialize(const context::InitParams& init)
{
    if (init.width == 0 || init.height == 0) return kBadShape;
    width = init.width;
    height = init.height;
    format = static_cast<DXGI_FORMAT>(init.format);
    depthInverted = (init.flags & context::kFlagDepthInverted) != 0;
    jittered = (init.flags & context::kFlagMotionJittered) != 0;
    transfer = TransferFor(format);
    LogOnce("FSR: color format " + std::to_string(init.format) + " uses the " + transfer.name
        + " transfer function");
    return CreateOpticalFlow() ? 0 : kOpticalFlowContext;
}

void Instance::ResolveCamera(const FrameExtras& extras)
{
    const bool hasNear = Usable(extras.cameraNear);
    const bool hasFar = Usable(extras.cameraFar);
    const bool hasFov = Usable(extras.cameraFovY);
    if (hasNear) camera.nearPlane = extras.cameraNear;
    if (hasFar) camera.farPlane = extras.cameraFar;
    if (hasFov) camera.fovY = extras.cameraFovY;
    if (!(hasNear && hasFar && hasFov))
        LogOnce("FSR: the game gave no usable camera near, far or field of view; using near 0.1, an infinite far "
                "plane and 1.0472 rad where missing");
}

const char* Instance::RefusalReason(const context::ExecuteParams& execute) const
{
    const auto& next = execute.next;
    if (execute.firstFrame >= execute.outputCount) return "FSR: refused, the frame index is past the output count";
    if (!next.depth.resource || !next.motion.resource || !next.backbuffer.resource || !execute.outputs
        || !execute.outputs[execute.firstFrame])
        return "FSR: refused, an input or output resource is missing";
    if (HasOffset(next.depth) || HasOffset(next.motion))
        return "FSR: refused, depth or motion vectors come with an input offset";
    if (next.depth.width == 0 || next.depth.height == 0 || next.depth.width > width || next.depth.height > height)
        return "FSR: refused, the render size is outside the output size";
    if (interpolationReady && (next.depth.width != renderWidth || next.depth.height != renderHeight))
        return "FSR: refused, the render size changed after the context was created";
    return nullptr;
}

bool Instance::TakeHistory(const ID3D12Resource* color)
{
    prepared = false;
    const bool colorRepeated = std::exchange(lastColor, color) == color;
    return std::exchange(historyValid, false) && !colorRepeated;
}

bool Instance::Prepare(ID3D12GraphicsCommandList* list, const context::FrameData& next, const FrameExtras& extras)
{
    FfxFrameInterpolationPrepareDescription prepare{};
    prepare.commandList = ffxGetCommandListDX12(list);
    prepare.renderSize = {renderWidth, renderHeight};
    prepare.jitterOffset = {next.jitter[0], next.jitter[1]};
    prepare.motionVectorScale = {next.motionScale[0], next.motionScale[1]};
    prepare.frameTimeDelta = FrameTime(extras);
    prepare.cameraNear = camera.nearPlane;
    prepare.cameraFar = camera.farPlane;
    prepare.viewSpaceToMetersFactor = 1.f;
    prepare.cameraFovAngleVertical = camera.fovY;
    prepare.depth = Input(next.depth.resource, L"Depth");
    prepare.motionVectors = Input(next.motion.resource, L"MotionVectors");
    prepare.frameID = frameID;
    prepare.dilatedDepth = Shared(dilatedDepth);
    prepare.dilatedMotionVectors = Shared(dilatedMotionVectors);
    prepare.reconstructedPrevDepth = Shared(reconstructedDepth);
    return Succeeded(ffxFrameInterpolationPrepare(&interpolation, &prepare), "frame interpolation prepare");
}

bool Instance::DispatchOpticalFlow(ID3D12GraphicsCommandList* list, const context::FrameData& next)
{
    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = ffxGetCommandListDX12(list);
    flow.color = Input(next.backbuffer.resource, L"Color");
    flow.reset = pairReset;
    flow.backbufferTransferFunction = transfer.function;
    flow.minMaxLuminance = {transfer.minLuminance, transfer.maxLuminance};
    flow.opticalFlowVector = Shared(flowVector);
    flow.opticalFlowSCD = Shared(flowSceneChange);
    return Succeeded(ffxOpticalflowContextDispatch(&opticalFlow, &flow), "optical flow dispatch");
}

bool Instance::EnsureInterpolation(const context::FrameData& next)
{
    if (interpolationReady || interpolationFailed) return interpolationReady;
    renderWidth = next.depth.width;
    renderHeight = next.depth.height;
    interpolationReady = CreateInterpolation(camera.farPlane == FLT_MAX);
    interpolationFailed = !interpolationReady;
    return interpolationReady;
}

int Instance::BeginPair(ID3D12GraphicsCommandList* list, const context::FrameData& next, const FrameExtras& extras,
    bool reset)
{
    ++gPairs;
    if (reset) ++gResets;
    pairReset = reset;
    ++frameID;
    if (!EnsureInterpolation(next)) return kInterpolationContext;
    if (!Prepare(list, next, extras)) return kPrepare;
    if (!DispatchOpticalFlow(list, next)) return kOpticalFlowDispatch;
    prepared = true;
    return 0;
}

int Instance::GenerateFrame(ID3D12GraphicsCommandList* list, const context::ExecuteParams& execute,
    const FrameExtras& extras)
{
    const uint32_t index = execute.firstFrame;
    const uint32_t count = execute.outputCount;
    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListDX12(list);
    dispatch.displaySize = {width, height};
    dispatch.renderSize = {renderWidth, renderHeight};
    dispatch.currentBackBuffer = Input(execute.next.backbuffer.resource, L"Color");
    dispatch.output = Output(execute.outputs[index], L"Output");
    dispatch.interpolationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    dispatch.opticalFlowVector = Shared(flowVector);
    dispatch.opticalFlowSceneChangeDetection = Shared(flowSceneChange);
    dispatch.opticalFlowBlockSize = kOpticalFlowBlockSize;
    dispatch.opticalFlowScale = {1.f / static_cast<float>(width), 1.f / static_cast<float>(height)};
    dispatch.cameraNear = camera.nearPlane;
    dispatch.cameraFar = camera.farPlane;
    dispatch.cameraFovAngleVertical = camera.fovY;
    dispatch.viewSpaceToMetersFactor = 1.f;
    dispatch.frameTimeDelta = FrameTime(extras);
    dispatch.reset = pairReset;
    dispatch.backBufferTransferFunction = transfer.function;
    dispatch.minMaxLuminance[0] = transfer.minLuminance;
    dispatch.minMaxLuminance[1] = transfer.maxLuminance;
    dispatch.frameID = frameID;
    dispatch.dilatedDepth = Shared(dilatedDepth);
    dispatch.dilatedMotionVectors = Shared(dilatedMotionVectors);
    dispatch.reconstructedPrevDepth = Shared(reconstructedDepth);
    dispatch.interpolationT = InterpolationInstant(index, count);
    dispatch.generatedFrameIndex = index;
    dispatch.generatedFrameCount = count;
    if (!Succeeded(ffxFrameInterpolationDispatch(&interpolation, &dispatch), "frame interpolation dispatch"))
        return kInterpolationDispatch;
    ++gDispatches;
    if (index + 1 == count) historyValid = true;
    return 0;
}

int Instance::Run(ID3D12GraphicsCommandList* list, const context::ExecuteParams& execute, const FrameExtras& extras)
{
    const bool pairStart = execute.firstFrame == 0;
    const bool historyLost = pairStart && !TakeHistory(execute.next.backbuffer.resource);
    if (const char* reason = RefusalReason(execute))
    {
        ++gRefusals;
        LogOnce(reason);
        return 1;
    }
    if (!pairStart && !prepared) return 1;
    ResolveCamera(extras);
    if (pairStart)
    {
        const int begun = BeginPair(list, execute.next, extras, execute.next.reset != 0 || historyLost);
        if (begun != 0) return begun;
    }
    return GenerateFrame(list, execute, extras);
}

bool Create(const Pair&, ID3D12Device* device, void*& handle, std::string& failure) noexcept
try
{
    auto instance = std::make_unique<Instance>();
    if (!instance->CreateBackends(device))
    {
        failure = "FSR: the FidelityFX backend did not initialize";
        return false;
    }
    handle = instance.release();
    return true;
}
catch (...)
{
    failure = "FSR: an exception stopped the context creation";
    return false;
}

int Initialize(const Pair&, void* handle, context::InitParams& init) noexcept
try
{
    return static_cast<Instance*>(handle)->Initialize(init);
}
catch (...)
{
    return kException;
}

int Execute(const Pair&, void* handle, ID3D12GraphicsCommandList* list, context::ExecuteParams& execute,
    const FrameExtras& extras) noexcept
try
{
    return static_cast<Instance*>(handle)->Run(list, execute, extras);
}
catch (...)
{
    return kException;
}
}

const xmx::generator::Model& Model() noexcept
{
    static const xmx::generator::Model model{"fsr", xmx::kMaxFrames, Create, Initialize, Execute};
    return model;
}

Stats ReadStats() noexcept
{
    return {gPairs.load(), gDispatches.load(), gResets.load(), gRefusals.load()};
}
}
