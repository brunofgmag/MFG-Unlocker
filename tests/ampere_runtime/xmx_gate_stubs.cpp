#include "xmx_gate_fakes.h"

#include "ampere_flip_metering.h"
#include "ampere_gpu.h"
#include "ampere_provider_arch.h"
#include "dlss5.h"
#include "dlss5_d3d11.h"
#include "dlss5_vulkan.h"
#include "dlss_sr.h"
#include "dlssg_preset.h"
#include "dlssg_provider_policy.h"
#include "dlssg_provider_selection.h"
#include "gpu_dispatch.h"
#include "midpoint_fix.h"
#include "xess/d3d12_host.h"
#include "xess/vulkan/vk_host.h"
#include "xmx/xmx.h"

XmxFake gXmx;
GateFake gGate;

namespace xmx
{
bool Latched() noexcept
{
    return gXmx.latched;
}

void NoteProviderRoute(bool installed) noexcept
{
    ++(installed ? gXmx.routesInstalled : gXmx.routesFailed);
}

bool AnswerNgxCall(NgxLayer layer, ProviderCall call, const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    ++(layer == NgxLayer::eRuntime ? gXmx.runtimeLayerCalls : gXmx.providerLayerCalls);
    switch (call)
    {
    case ProviderCall::eCreate:
        if (args.featureOrHandle != NVSDK_NGX_Feature_FrameGeneration || !gXmx.latched) return false;
        ++gXmx.createCalls;
        gXmx.createList = args.commandList;
        if (!gXmx.answersCreate) return false;
        *static_cast<NVSDK_NGX_Handle**>(args.outHandle) = gXmx.createHandle;
        result = gXmx.createResult;
        return true;
    case ProviderCall::eEvaluate:
        if (!gXmx.latched) return false;
        ++gXmx.evaluateCalls;
        gXmx.list = args.commandList;
        gXmx.parameters = args.parameters;
        if (gXmx.handles) result = gXmx.result;
        return gXmx.handles;
    case ProviderCall::eRelease:
        ++gXmx.releaseCalls;
        gXmx.released = reinterpret_cast<const void*>(args.featureOrHandle);
        if (gXmx.answersRelease) result = gXmx.releaseResult;
        return gXmx.answersRelease;
    }
    return false;
}
}

namespace gpu_dispatch
{
Family Selected() noexcept { return Family::eAda; }
bool UsesNativeProgram() noexcept { return true; }
bool UsesTranslatedProgram() noexcept { return false; }
bool ObserveD3D12Device(void*) noexcept { return false; }
}

namespace midpoint_fix
{
bool OutputPullMaskRequiresRestart() noexcept { return gGate.pullMaskRestart; }
}

namespace dlssg_provider_policy
{
bool IsSupportedProvider(HMODULE, const wchar_t*) noexcept { return !gGate.providerRefused; }
bool IsSupportedAmpereProvider(HMODULE, const wchar_t*) noexcept { return !gGate.providerRefused; }
bool ReadProviderVersion(const wchar_t*, VersionTriplet&) noexcept { return false; }
}

namespace dlssg_provider_selection
{
Snapshot Read() noexcept { return {}; }
}

namespace dlssg_preset
{
uint32_t FreezeSelection() noexcept { return 0; }
uint32_t EffectiveSelection(uint32_t preset, HMODULE) noexcept { return preset; }
void RecordProviderRead(HMODULE, bool, uint32_t, bool) noexcept {}
void RecordRouteRead(HMODULE, const void*, bool, uint32_t, bool) noexcept {}
bool AdoptRouteReader(HMODULE, void*, uint64_t, entry_detour::Handle) noexcept { return false; }
void* FindPresetReader(HMODULE) noexcept { return nullptr; }
}

namespace ampere_provider_arch
{
bool Armed() noexcept { return false; }
Outcome Declare(HMODULE) noexcept { return Outcome::eNotArmed; }
const char* OutcomeName(Outcome) noexcept { return ""; }
}

namespace ampere_flip_metering
{
bool Active() noexcept { return false; }
bool Observing() noexcept { return false; }
}

namespace ampere_gpu
{
void SetLogCallback(LogCallback) noexcept {}
bool ObserveD3D12Device(void*) noexcept { return false; }
bool ObserveAdapter(void*) noexcept { return false; }
bool ObserveVulkanPhysicalDevice(void*) noexcept { return false; }
bool ObserveVulkanAdapter(void*) noexcept { return false; }
bool PatchProvider(HMODULE, const wchar_t*, const PreparationBoundary&) noexcept { return false; }
bool PreparedProvider(HMODULE) noexcept { return false; }
bool BindProvider(HMODULE) noexcept { return false; }
bool AdapterVerified() noexcept { return false; }
bool Ready() noexcept { return false; }
uint32_t FailureCode() noexcept { return 0; }
uint32_t PreparationStage() noexcept { return 0; }
uint64_t AdapterLuid() noexcept { return 0; }
uint64_t Publication() noexcept { return 0; }
HMODULE Provider() noexcept { return nullptr; }
}

namespace dlss_sr
{
bool Active() noexcept { return false; }
NVSDK_NGX_Result Create(size_t, const CreateCall&, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**) noexcept
{
    return NVSDK_NGX_Result_Fail;
}
const NVSDK_NGX_Handle* Evaluate(size_t, const CreateCall&, ReleaseFn, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter*) noexcept
{
    return handle;
}
void BeforeRelease(size_t, ReleaseFn, NVSDK_NGX_Handle*) noexcept {}
void AfterRelease(size_t, NVSDK_NGX_Handle*, NVSDK_NGX_Result) noexcept {}
}

namespace dlss5
{
dlss_sr::CreateKeys ReadCreate(const NVSDK_NGX_Parameter*) noexcept { return {}; }
void GameCreated(size_t, const NVSDK_NGX_Handle*, const dlss_sr::CreateKeys&) noexcept {}
void ForeignCreated(const void*, HMODULE) noexcept {}
bool Owns(size_t, const NVSDK_NGX_Handle*) noexcept { return false; }
Substitution BeforeEvaluate(size_t, const Api&, ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, uint64_t) noexcept
{
    return {};
}
void AfterEvaluate(size_t, const Api&, ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Result, const Substitution&, uint64_t) noexcept {}
void BeforeRelease(size_t, const NVSDK_NGX_Handle*) noexcept {}
void AfterRelease(size_t, const NVSDK_NGX_Handle*, NVSDK_NGX_Result) noexcept {}
}

namespace dlss5::d3d11
{
void GameCreated(const NVSDK_NGX_Handle*, const dlss_sr::CreateKeys&) noexcept {}
void UntrackedCreate() noexcept {}
NVSDK_NGX_Result Evaluate(const Api& api, ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) noexcept
{
    return api.evaluate ? api.evaluate(context, handle, parameters, callback) : NVSDK_NGX_Result_Fail;
}
bool BeforeRelease(const NVSDK_NGX_Handle*) noexcept { return false; }
void AfterRelease(const NVSDK_NGX_Handle*, NVSDK_NGX_Result, bool) noexcept {}
}

namespace dlss5::vulkan
{
void GameCreated(size_t, const NVSDK_NGX_Handle*, const dlss_sr::CreateKeys&, void*) noexcept {}
void AfterEvaluate(size_t, const Api&, void*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Result) noexcept {}
void BeforeRelease(size_t, const NVSDK_NGX_Handle*) noexcept {}
void AfterRelease(size_t, const NVSDK_NGX_Handle*, NVSDK_NGX_Result) noexcept {}
}

namespace xess_d3d12
{
void CaptureDlssEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Parameter*) noexcept {}
}

namespace xess_vulkan
{
bool CaptureDlssEvaluate(void*, const NVSDK_NGX_Parameter*) noexcept { return false; }
}
