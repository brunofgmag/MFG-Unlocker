#include "ampere_backend.h"
#include "xmx_gate_fakes.h"

#include <Windows.h>
#include <nvsdk_ngx.h>

#include <cstdio>

namespace
{
unsigned gChecks = 0, gFailures = 0;

void Check(bool ok, const char* message)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    std::fflush(stdout);
}

using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, const void*, const void*, void*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, int, const void*, void*);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*);

constexpr NVSDK_NGX_Result kOriginalEvaluate = static_cast<NVSDK_NGX_Result>(4661);
constexpr NVSDK_NGX_Result kOriginalCreate = static_cast<NVSDK_NGX_Result>(4660);
constexpr NVSDK_NGX_Result kOriginalRelease = static_cast<NVSDK_NGX_Result>(4662);
constexpr NVSDK_NGX_Result kGeneratorFailure = NVSDK_NGX_Result_FAIL_PlatformError;
constexpr NVSDK_NGX_Result kRejection = NVSDK_NGX_Result_FAIL_FeatureNotSupported;

volatile long long* gCalls = nullptr;
EvaluateFn gEvaluate = nullptr;
CreateFn gCreate = nullptr;
ReleaseFn gRelease = nullptr;
int gListMarker = 0, gParameterMarker = 0, gHandleMarker = 0;

struct RuntimeFixture
{
    volatile long long* calls = nullptr;
    EvaluateFn evaluate = nullptr;
    CreateFn create = nullptr;
    ReleaseFn release = nullptr;
};
RuntimeFixture gRuntime;

void WINAPI CountCreate(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t, entry_detour::Handle,
    const void*) noexcept
{
    ++gGate.beforeCreateCalls;
}

void WINAPI CountEvaluate(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t, entry_detour::Handle,
    const void*) noexcept
{
    ++gGate.beforeEvaluateCalls;
}

void Reset()
{
    gXmx = {};
    gGate = {};
    gCalls[2] = 0;
    gCalls[1] = 0;
    gCalls[0] = 0;
    if (!gRuntime.calls) return;
    gRuntime.calls[2] = 0;
    gRuntime.calls[1] = 0;
    gRuntime.calls[0] = 0;
}

NVSDK_NGX_Result Evaluate()
{
    return gEvaluate(&gListMarker, &gHandleMarker, &gParameterMarker, nullptr);
}

NVSDK_NGX_Result RuntimeEvaluate()
{
    return gRuntime.evaluate(&gListMarker, &gHandleMarker, &gParameterMarker, nullptr);
}

NVSDK_NGX_Result RuntimeCreate(NVSDK_NGX_Feature feature, NVSDK_NGX_Handle** created)
{
    return gRuntime.create(&gListMarker, feature, &gParameterMarker, created);
}

bool LoadRuntimeFixture()
{
    HMODULE fixture = LoadLibraryW(L"XmxGateRuntimeFixture.dll");
    if (!fixture) return false;
    gRuntime.calls = reinterpret_cast<volatile long long*>(GetProcAddress(fixture, "GateFixtureCalls"));
    gRuntime.evaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(fixture, "NVSDK_NGX_D3D12_EvaluateFeature"));
    gRuntime.create = reinterpret_cast<CreateFn>(GetProcAddress(fixture, "NVSDK_NGX_D3D12_CreateFeature"));
    gRuntime.release = reinterpret_cast<ReleaseFn>(GetProcAddress(fixture, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!gRuntime.calls || !gRuntime.evaluate || !gRuntime.create || !gRuntime.release) return false;
    return ampere_backend::InstallRoute(fixture, L"C:\\fixture\\_nvngx.dll", 2, true, &CountCreate, &CountEvaluate);
}

void RuntimeCreateCases()
{
    Reset();
    gXmx.latched = true;
    gXmx.answersCreate = true;
    gXmx.createHandle = reinterpret_cast<NVSDK_NGX_Handle*>(&gHandleMarker);
    const auto before = ampere_backend::ReadFrameGenerationLiveness();
    NVSDK_NGX_Handle* created = nullptr;
    Check(RuntimeCreate(NVSDK_NGX_Feature_FrameGeneration, &created) == NVSDK_NGX_Result_Success,
        "runtime route: XMX answers the Create and the answered result comes back");
    Check(gRuntime.calls[0] == 0 && created == gXmx.createHandle,
        "runtime route: the runtime's own Create never runs and the handle is in the out parameter");
    Check(gXmx.createList == &gListMarker && gGate.beforeCreateCalls == 1,
        "runtime route: XMX gets the command list and the route's pre-call still counted the Create");
    Check(gXmx.runtimeLayerCalls == 1 && gXmx.providerLayerCalls == 0,
        "runtime route: the Create is offered to XMX at the runtime layer only");
    const auto after = ampere_backend::ReadFrameGenerationLiveness();
    Check(after.trackedCreates == before.trackedCreates && after.liveFeatures == before.liveFeatures
            && after.createsInFlight == 0,
        "runtime route: a stand-in handle never enters the lifetime tracker");

    created = nullptr;
    Check(gRuntime.create(nullptr, NVSDK_NGX_Feature_FrameGeneration, &gParameterMarker, &created)
            == NVSDK_NGX_Result_Success && created == gXmx.createHandle && gRuntime.calls[0] == 0,
        "runtime route: a Create with no command list is still answered");
    created = nullptr;
    Check(gRuntime.create(&gListMarker, NVSDK_NGX_Feature_FrameGeneration, nullptr, &created)
            == NVSDK_NGX_Result_Success && created == gXmx.createHandle && gRuntime.calls[0] == 0,
        "runtime route: a Create with no parameters is still answered");

    gXmx.answersCreate = false;
    created = nullptr;
    Check(RuntimeCreate(NVSDK_NGX_Feature_FrameGeneration, &created) == kOriginalCreate && gRuntime.calls[0] == 1
            && created == nullptr,
        "runtime route: XMX declines the Create and the runtime's original runs");

    gXmx.answersCreate = true;
    const int offered = gXmx.runtimeLayerCalls;
    Check(RuntimeCreate(NVSDK_NGX_Feature_DeepDVC, &created) == kOriginalCreate && gXmx.runtimeLayerCalls == offered,
        "runtime route: another feature's Create is never offered to XMX");

    gGate.pullMaskRestart = true;
    Check(RuntimeCreate(NVSDK_NGX_Feature_FrameGeneration, &created) == kRejection
            && gXmx.runtimeLayerCalls == offered,
        "runtime route: a rejection that is not XMX's keeps the rejection and never reaches XMX");
    gGate.pullMaskRestart = false;
}

void RuntimeEvaluateCases()
{
    Reset();
    gXmx.latched = true;
    gXmx.handles = true;
    gXmx.result = kGeneratorFailure;
    Check(RuntimeEvaluate() == kGeneratorFailure && gRuntime.calls[1] == 0,
        "runtime route: XMX answers the Evaluate and the runtime's original never runs");
    Check(gXmx.evaluateCalls == 1 && gXmx.list == &gListMarker && gXmx.parameters == &gParameterMarker,
        "runtime route: XMX gets the command list and the parameters");
    Check(gGate.beforeEvaluateCalls == 1 && gXmx.runtimeLayerCalls == 1 && gXmx.providerLayerCalls == 0,
        "runtime route: the pre-call counted the Evaluate and XMX was asked at the runtime layer only");

    Check(gRuntime.evaluate(nullptr, &gHandleMarker, &gParameterMarker, nullptr) == kGeneratorFailure
            && gRuntime.calls[1] == 0,
        "runtime route: an Evaluate with no command list is still answered");
    Check(gRuntime.evaluate(&gListMarker, &gHandleMarker, nullptr, nullptr) == kGeneratorFailure
            && gRuntime.calls[1] == 0,
        "runtime route: an Evaluate with no parameters is still answered");
    Check(gRuntime.evaluate(&gListMarker, &gHandleMarker, &gParameterMarker, &gListMarker) == kGeneratorFailure
            && gRuntime.calls[1] == 0,
        "runtime route: an Evaluate that carries a progress callback is still answered");

    gXmx.handles = false;
    Check(RuntimeEvaluate() == kOriginalEvaluate && gRuntime.calls[1] == 1,
        "runtime route: XMX declines the Evaluate and the runtime's original runs");
    Check(gXmx.evaluateCalls == 5 && gGate.beforeEvaluateCalls == 5,
        "runtime route: a declined Evaluate was offered to XMX and counted");
}

void RuntimeReleaseCases()
{
    Reset();
    gXmx.answersRelease = true;
    const auto before = ampere_backend::ReadFrameGenerationLiveness();
    Check(gRuntime.release(&gHandleMarker) == NVSDK_NGX_Result_Success && gRuntime.calls[2] == 0,
        "runtime route: XMX answers the Release and the runtime's original never runs");
    Check(gXmx.releaseCalls == 1 && gXmx.released == &gHandleMarker && gXmx.runtimeLayerCalls == 1
            && gXmx.providerLayerCalls == 0,
        "runtime route: XMX gets the released handle at the runtime layer only");
    const auto after = ampere_backend::ReadFrameGenerationLiveness();
    Check(after.trackedReleases == before.trackedReleases, "runtime route: an answered Release leaves the tracker alone");

    Check(gRuntime.release(nullptr) == NVSDK_NGX_Result_Success && gRuntime.calls[2] == 0 && gXmx.released == nullptr,
        "runtime route: a Release of nothing is offered to XMX like any other");

    gXmx.answersRelease = false;
    Check(gRuntime.release(&gHandleMarker) == kOriginalRelease && gRuntime.calls[2] == 1 && gXmx.releaseCalls == 3,
        "runtime route: XMX declines the Release and the runtime's original runs");
}

void RuntimeRouteCases()
{
    if (!LoadRuntimeFixture())
    {
        Check(false, "the runtime route installs on the second fixture's Create, Evaluate and Release");
        return;
    }
    Check(true, "the runtime route installs on the second fixture's Create, Evaluate and Release");
    RuntimeCreateCases();
    RuntimeEvaluateCases();
    RuntimeReleaseCases();
}
}

void ProviderRouteNotes(HMODULE fixture)
{
    constexpr wchar_t kProviderPath[] = L"C:\\fixture\\nvngx_dlssg.dll";
    const HMODULE foreign = GetModuleHandleW(L"kernel32.dll");

    Reset();
    Check(ampere_backend::InstallRoute(fixture, kProviderPath, 1, false, &CountCreate, &CountEvaluate)
            && gXmx.routesInstalled == 1 && gXmx.routesFailed == 0,
        "a provider route that installs is noted as installed");

    Reset();
    gGate.providerRefused = true;
    Check(!ampere_backend::InstallRoute(foreign, kProviderPath, 3, false, &CountCreate, &CountEvaluate)
            && gXmx.routesInstalled == 0 && gXmx.routesFailed == 1,
        "a provider the policy refuses is noted as a failed route");

    Reset();
    Check(!ampere_backend::InstallRoute(foreign, kProviderPath, 3, false, &CountCreate, &CountEvaluate)
            && gXmx.routesInstalled == 0 && gXmx.routesFailed == 1,
        "a provider whose exports cannot be routed is noted as a failed route");

    Reset();
    Check(!ampere_backend::InstallRoute(foreign, kProviderPath, 3, true, &CountCreate, &CountEvaluate)
            && gXmx.routesInstalled == 0 && gXmx.routesFailed == 0,
        "a runtime route that fails is not a provider route and is not noted");

    for (uint64_t generation = 100; generation < 108; ++generation)
        ampere_backend::InstallRoute(foreign, kProviderPath, generation, true, &CountCreate, &CountEvaluate);
    Reset();
    Check(!ampere_backend::InstallRoute(fixture, kProviderPath, 999, false, &CountCreate, &CountEvaluate)
            && gXmx.routesInstalled == 0 && gXmx.routesFailed == 1,
        "a provider with no route left is noted as a failed route");
}

int wmain()
{
    HMODULE fixture = LoadLibraryW(L"XmxGateFixture.dll");
    gCalls = fixture ? reinterpret_cast<volatile long long*>(GetProcAddress(fixture, "GateFixtureCalls")) : nullptr;
    gEvaluate = fixture ? reinterpret_cast<EvaluateFn>(GetProcAddress(fixture, "NVSDK_NGX_D3D12_EvaluateFeature"))
                        : nullptr;
    gCreate = fixture ? reinterpret_cast<CreateFn>(GetProcAddress(fixture, "NVSDK_NGX_D3D12_CreateFeature")) : nullptr;
    gRelease = fixture ? reinterpret_cast<ReleaseFn>(GetProcAddress(fixture, "NVSDK_NGX_D3D12_ReleaseFeature")) : nullptr;
    if (!gCalls || !gEvaluate || !gCreate || !gRelease)
    {
        std::printf("setup: XmxGateFixture.dll exports unavailable (%lu)\n", GetLastError());
        return 2;
    }
    Check(gEvaluate(nullptr, nullptr, nullptr, nullptr) == kOriginalEvaluate && gCalls[1] == 1,
        "before the route the fixture's own Evaluate runs");
    Check(ampere_backend::InstallRoute(fixture, L"C:\\fixture\\nvngx_dlssg.dll", 1, false, &CountCreate,
              &CountEvaluate),
        "the provider route installs on the fixture's Create, Evaluate and Release");

    Reset();
    gXmx.latched = true;
    gXmx.handles = true;
    gXmx.result = kGeneratorFailure;
    Check(Evaluate() == kGeneratorFailure, "XMX latched and handled: the generator's own result comes back");
    Check(gCalls[1] == 0, "XMX handled: the original Evaluate never runs");
    Check(gXmx.evaluateCalls == 1 && gXmx.list == &gListMarker && gXmx.parameters == &gParameterMarker,
        "XMX gets the command list and the parameters of the call");
    Check(gGate.beforeEvaluateCalls == 1, "XMX handled: the route's own pre-call still counted the Evaluate");
    Check(gXmx.providerLayerCalls == 1 && gXmx.runtimeLayerCalls == 0,
        "the provider route offers the Evaluate to XMX at the provider layer only");

    gXmx.handles = false;
    Check(Evaluate() == kOriginalEvaluate && gCalls[1] == 1, "XMX latched and declined: NVIDIA's original runs");
    Check(gXmx.evaluateCalls == 2 && gGate.beforeEvaluateCalls == 2, "a declined call was offered to XMX and counted");

    gXmx.handles = true;
    gXmx.result = NVSDK_NGX_Result_Success;
    gGate.pullMaskRestart = true;
    Check(Evaluate() == kRejection && gXmx.evaluateCalls == 2,
        "a rejection that is not XMX's keeps the rejection and never reaches XMX");
    gGate.pullMaskRestart = false;
    Check(Evaluate() == NVSDK_NGX_Result_Success && gCalls[1] == 1, "a handled result is not left over after a rejection");

    Reset();
    gXmx.handles = true;
    gXmx.result = kGeneratorFailure;
    Check(Evaluate() == kOriginalEvaluate && gCalls[1] == 1 && gXmx.evaluateCalls == 0,
        "XMX off: the original runs and XMX is never asked");
    Check(gGate.beforeEvaluateCalls == 1, "XMX off: the pre-call counts the Evaluate as before");

    Reset();
    gXmx.latched = true;
    Check(gCreate(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, nullptr) == kOriginalCreate && gCalls[0] == 1,
        "a frame generation Create still reaches NVIDIA's Create");
    Check(gXmx.createCalls == 1 && gGate.beforeCreateCalls == 1, "XMX is told of the first frame generation Create");
    Check(gCreate(nullptr, NVSDK_NGX_Feature_SuperSampling, nullptr, nullptr) == kOriginalCreate && gXmx.createCalls == 1,
        "another feature's Create does not wake XMX");

    Reset();
    Check(gCreate(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, nullptr) == kOriginalCreate && gXmx.createCalls == 0,
        "XMX off: a Create never touches XMX");

    Reset();
    gXmx.latched = true;
    gXmx.answersCreate = true;
    gXmx.createResult = NVSDK_NGX_Result_Success;
    gXmx.createHandle = reinterpret_cast<NVSDK_NGX_Handle*>(&gHandleMarker);
    NVSDK_NGX_Handle* created = nullptr;
    Check(gCreate(&gListMarker, NVSDK_NGX_Feature_FrameGeneration, &gParameterMarker, &created)
            == NVSDK_NGX_Result_Success,
        "XMX answers the Create: the answered result comes back");
    Check(gCalls[0] == 0, "XMX answers the Create: NVIDIA's original Create never runs");
    Check(created == gXmx.createHandle, "XMX answers the Create: its handle is in the out parameter");
    Check(gXmx.createList == &gListMarker && gGate.beforeCreateCalls == 1,
        "XMX gets the Create's command list and the route's own pre-call still counted it");
    Check(gXmx.providerLayerCalls == 1 && gXmx.runtimeLayerCalls == 0,
        "the provider route offers the Create to XMX at the provider layer only");

    gXmx.answersCreate = false;
    created = nullptr;
    Check(gCreate(&gListMarker, NVSDK_NGX_Feature_FrameGeneration, &gParameterMarker, &created) == kOriginalCreate
            && gCalls[0] == 1 && created == nullptr,
        "XMX declines the Create: NVIDIA's original runs and no handle is written");

    gXmx.answersCreate = true;
    gGate.pullMaskRestart = false;
    gCreate(&gListMarker, NVSDK_NGX_Feature_FrameGeneration, &gParameterMarker, &created);
    gXmx.handles = true;
    gXmx.result = kGeneratorFailure;
    gGate.pullMaskRestart = true;
    Check(Evaluate() == kRejection && gXmx.evaluateCalls == 0,
        "a handled Create leaves nothing over for the next Evaluate");
    gGate.pullMaskRestart = false;

    Reset();
    gXmx.answersRelease = true;
    gXmx.releaseResult = NVSDK_NGX_Result_Success;
    Check(gRelease(&gHandleMarker) == NVSDK_NGX_Result_Success && gCalls[2] == 0,
        "XMX answers the Release: Success comes back and NVIDIA's original Release never runs");
    Check(gXmx.releaseCalls == 1 && gXmx.released == &gHandleMarker, "XMX gets the released handle");
    Check(gXmx.providerLayerCalls == 1 && gXmx.runtimeLayerCalls == 0,
        "the provider route offers the Release to XMX at the provider layer only");
    gXmx.latched = true;
    gXmx.handles = true;
    gGate.pullMaskRestart = true;
    Check(Evaluate() == kRejection, "a handled Release leaves nothing over for the next Evaluate");
    gGate.pullMaskRestart = false;

    gXmx.answersRelease = false;
    Check(gRelease(&gHandleMarker) == kOriginalRelease && gCalls[2] == 1 && gXmx.releaseCalls == 2,
        "XMX declines the Release: NVIDIA's original runs");

    RuntimeRouteCases();
    ProviderRouteNotes(fixture);

    std::printf("XMX_GATE checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
