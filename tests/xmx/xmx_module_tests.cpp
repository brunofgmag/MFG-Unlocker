#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

#include "gpu_dispatch.h"
#include "single_module.h"
#include "xess/xess_mod.h"
#include "xmx/xmx.h"
#include "xmx/xmx_generator.h"
#include "xmx/xmx_stand_in.h"

namespace
{
unsigned gChecks = 0, gFailures = 0;
HMODULE gSelf = nullptr;
unsigned gAttachCalls = 0;
xmx::generator::Pair gAttached{};
HMODULE gPublishedFg = nullptr, gPublishedLl = nullptr;
bool gGeneratorAnswer = true;
unsigned gGeneratorEvaluates = 0;
bool gPassthroughAnswer = true;
unsigned gPassthroughCalls = 0;
unsigned gLimitLogs = 0;
bool gTranslatedGpu = false;
unsigned gDeviceQueries = 0;
unsigned gUsedLineCalls = 0;
constexpr xmx::NgxLayer Runtime = xmx::NgxLayer::eRuntime;
constexpr xmx::NgxLayer Provider = xmx::NgxLayer::eProvider;

class FakeList : public ID3D12DeviceChild
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override
    {
        ++gDeviceQueries;
        return E_FAIL;
    }
};

FakeList gList;

class FakeParameters : public NVSDK_NGX_Parameter
{
public:
    explicit FakeParameters(int enableInterp, bool present = true) : enableInterp_(enableInterp), present_(present) {}
    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned int) override {}
    void Set(const char*, int) override {}
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, unsigned int*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char* name, int* value) const override
    {
        if (!present_ || std::string(name) != "DLSSG.EnableInterp") return NVSDK_NGX_Result_Fail;
        *value = enableInterp_;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override {}

private:
    int enableInterp_;
    bool present_;
};

char gSentinelByte = 0;
NVSDK_NGX_Handle* const kSentinel = reinterpret_cast<NVSDK_NGX_Handle*>(&gSentinelByte);
int gForeignByte = 0;
const void* const kForeign = &gForeignByte;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

NVSDK_NGX_Result EvaluateOnce(bool& handled)
{
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    handled = xmx::Evaluate(static_cast<ID3D12DeviceChild*>(&gList), reinterpret_cast<const void*>(1), result);
    return result;
}

struct Outcome
{
    bool answered = false;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Handle* handle = kSentinel;
};

void* ListPointer()
{
    return static_cast<ID3D12DeviceChild*>(&gList);
}

Outcome CreateFeature(xmx::NgxLayer layer, uintptr_t feature = NVSDK_NGX_Feature_FrameGeneration,
    void* list = ListPointer())
{
    Outcome outcome;
    outcome.answered = xmx::AnswerNgxCall(layer, xmx::ProviderCall::eCreate,
        {list, feature, nullptr, &outcome.handle}, outcome.result);
    return outcome;
}

Outcome EvaluateFeature(xmx::NgxLayer layer, const void* handle, const NVSDK_NGX_Parameter* parameters,
    void* list = ListPointer())
{
    Outcome outcome;
    outcome.answered = xmx::AnswerNgxCall(layer, xmx::ProviderCall::eEvaluate,
        {list, reinterpret_cast<uintptr_t>(handle), parameters, nullptr}, outcome.result);
    return outcome;
}

Outcome ReleaseFeature(xmx::NgxLayer layer, const void* handle)
{
    Outcome outcome;
    outcome.answered = xmx::AnswerNgxCall(layer, xmx::ProviderCall::eRelease,
        {nullptr, reinterpret_cast<uintptr_t>(handle), nullptr, nullptr}, outcome.result);
    return outcome;
}

unsigned IdOf(const NVSDK_NGX_Handle* handle)
{
    return handle == nullptr ? 0 : reinterpret_cast<const xmx::stand_in::Handle*>(handle)->Id;
}

HMODULE LoadFrom(const std::wstring& folder, const wchar_t* name, DWORD flags = LOAD_WITH_ALTERED_SEARCH_PATH)
{
    return LoadLibraryExW((folder + L"\\" + name).c_str(), nullptr, flags);
}

void RouteBeforeLatch()
{
    xmx::NoteProviderRoute(true);
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    const auto status = xmx::ReadStatus(true);
    Check(status.latched, "the status reports the latch");
    Check(status.hooked, "a provider route installed before the latch counts");
    Check(!status.partiallyHooked && status.failure.empty(), "one route installed is not a failure");
    Check(status.bypassed, "hooked, never evaluated and the driver generating is bypassed");
}

void NoRoute()
{
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    xmx::NoteProviderRoute(false);
    auto status = xmx::ReadStatus(true);
    Check(!status.hooked, "no route installed means not hooked");
    Check(status.failure == "the hook on NVIDIA's generator could not be installed",
        "no provider route installed is reported as a failure to hook");
    Check(!status.bypassed, "without a hook the driver's generation is not a bypass");
    xmx::NoteProviderRoute(true);
    status = xmx::ReadStatus(true);
    Check(status.hooked && status.partiallyHooked, "one route installed and one failed is partially hooked");
    Check(status.failure.empty(), "one installed route is no longer a failure");
    Check(status.bypassed, "hooked and never evaluated while the driver generates is a bypass");
}

void RoutesWithoutLatch()
{
    xmx::NoteProviderRoute(false);
    Check(xmx::Latch(false, false) == xmx::Selection::eOff, "XMX stays off");
    const auto status = xmx::ReadStatus(true);
    Check(!status.latched && status.failure.empty(), "an unlatched XMX reports no failure to hook");
}

void HookedWithoutLatch()
{
    xmx::NoteProviderRoute(true);
    Check(xmx::Latch(false, false) == xmx::Selection::eOff, "XMX stays off");
    const auto status = xmx::ReadStatus(true);
    Check(status.hooked, "a provider route is reported whether or not XMX is on");
    Check(!status.latched && !status.bypassed, "an unlatched XMX is never bypassed");
}

void EvaluateEntry()
{
    xmx::NoteProviderRoute(true);
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    auto status = xmx::ReadStatus(true);
    Check(!status.evaluating && status.bypassed, "before any call the generator is bypassed and not evaluating");
    bool handled = true;
    EvaluateOnce(handled);
    Check(!handled, "with no runtime the call is not handled");
    Check(gDeviceQueries == 0, "with no runtime the list's device is not asked for");
    status = xmx::ReadStatus(true);
    Check(status.evaluating, "a call that reached the hook counts as evaluating, runtime or not");
    Check(!status.bypassed, "a generator that was called is not bypassed");
    Check(gAttachCalls == 0, "no runtime was attached");
}

void TranslatedGpu()
{
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    Check(xmx::Latched(), "XMX is latched before the GPU is known");
    gTranslatedGpu = true;
    Check(!xmx::Latched(), "an RTX 30 or RTX 20 GPU unlatches XMX");
    const auto status = xmx::ReadStatus(false);
    Check(!status.latched && status.refusal == "translated_gpu", "the status names the refusal");
    bool handled = true;
    EvaluateOnce(handled);
    Check(!handled, "a refused XMX does not handle the call");
    Check(!xmx::ReadStatus(false).evaluating, "a refused XMX does not count calls");
    Check(gDeviceQueries == 0, "a refused XMX does not ask for the device");
    xmx::BeforeCreate();
    Check(gAttachCalls == 0, "a refused XMX loads no runtime");
}

void TranslatedGpuFirst()
{
    gTranslatedGpu = true;
    Check(xmx::Latch(true, false) == xmx::Selection::eRefusedTranslatedGpu, "the latch honors a GPU seen first");
    Check(!xmx::Latched() && xmx::ReadStatus(false).refusal == "translated_gpu", "the refusal is reported");
}

void StandaloneKeepsItsReason()
{
    Check(xmx::Latch(true, true) == xmx::Selection::eRefusedStandalone, "XeMFG standalone refuses XMX");
    gTranslatedGpu = true;
    Check(xmx::ReadStatus(false).refusal == "xemfg_standalone", "the first refusal keeps its reason");
}

void NativeGpuStaysLatched()
{
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    Check(xmx::Latched() && xmx::ReadStatus(false).refusal.empty(), "a GPU that is never reported stays latched");
}

void LoadedRuntime(const std::wstring& folder)
{
    const HMODULE ll = LoadFrom(folder, L"libxell.dll");
    const HMODULE fg = LoadFrom(folder, L"libxess_fg.dll");
    if (!Check(ll && fg, "the game's own libxell.dll and libxess_fg.dll load")) return;
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    xmx::BeforeCreate();
    Check(gAttachCalls == 1 && gAttached.fg == fg, "the generator attaches to the module the game loaded");
    Check(gPublishedFg == fg && gPublishedLl == ll, "the model is published on the game's own pair");
    Check(gAttached.rvas != nullptr && gAttached.maxFrames == xmx::kMaxFrames, "the pair is the fg78 family");
    bool handled = false;
    const auto result = EvaluateOnce(handled);
    Check(handled && result == NVSDK_NGX_Result_Success, "a ready runtime hands the call to the generator");
    Check(gDeviceQueries == 1, "a ready runtime asks the command list for its device once");
    Check(xmx::ReadStatus(false).failure.empty(), "nothing failed");
}

void LoadedRuntimeFromFolder(const std::wstring& folder)
{
    gSelf = LoadFrom(folder, L"libxess_dx11.dll", DONT_RESOLVE_DLL_REFERENCES);
    if (!Check(gSelf != nullptr, "a module in the runtime folder stands in for MFG Unlocker")) return;
    Check(GetModuleHandleW(L"libxess_fg.dll") == nullptr && GetModuleHandleW(L"libxell.dll") == nullptr,
        "the game loaded neither library");
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    xmx::BeforeCreate();
    const HMODULE fg = GetModuleHandleW(L"libxess_fg.dll"), ll = GetModuleHandleW(L"libxell.dll");
    Check(fg && ll, "the folder's pair was loaded");
    Check(gAttachCalls == 1 && gAttached.fg == fg, "the generator attaches to the folder's copy");
    Check(gPublishedFg == fg && gPublishedLl == ll, "the model is published on the folder's pair");
}

void LoadedRuntimeNotFg78(const std::wstring& folder)
{
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    const std::wstring copy = std::wstring(temporary) + L"xmx-module-tests-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(copy.c_str(), nullptr);
    const bool copied = CopyFileW((folder + L"\\libxell.dll").c_str(), (copy + L"\\libxess_fg.dll").c_str(), FALSE);
    const HMODULE ll = LoadFrom(folder, L"libxell.dll");
    const HMODULE fake = LoadFrom(copy, L"libxess_fg.dll");
    if (!Check(copied && ll && fake, "a library that is not XeFG loads under libxess_fg.dll's name")) return;
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    xmx::BeforeCreate();
    Check(gAttachCalls == 0, "a loaded copy outside the fg78 family is not attached");
    Check(!xmx::ReadStatus(false).failure.empty(), "the refusal is reported as a failure");
    bool handled = true;
    EvaluateOnce(handled);
    Check(!handled, "the call is not handled");
    DeleteFileW((copy + L"\\libxess_fg.dll").c_str());
    RemoveDirectoryW(copy.c_str());
}

bool LoadGamePair(const std::wstring& folder)
{
    const HMODULE ll = LoadFrom(folder, L"libxell.dll");
    const HMODULE fg = LoadFrom(folder, L"libxess_fg.dll");
    return Check(ll && fg, "the game's own libxell.dll and libxess_fg.dll load");
}

void StandInKeyOff(const std::wstring& folder)
{
    if (!LoadGamePair(folder)) return;
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    auto outcome = CreateFeature(Runtime, NVSDK_NGX_Feature_SuperSampling);
    Check(!outcome.answered && outcome.handle == kSentinel && gAttachCalls == 0,
        "another feature's runtime create is not answered and wakes nothing");
    outcome = CreateFeature(Runtime);
    Check(!outcome.answered, "a frame generation runtime create with the key off is forwarded to NVIDIA");
    Check(outcome.handle == kSentinel && xmx::StandInLive() == 0, "the key off writes no handle");
    Check(gAttachCalls == 1, "the runtime create still prepares the runtime, as the gate did before the stand-in");
    Check(gUsedLineCalls == 0, "a runtime create NVIDIA will answer notes no memory, the provider layer does");
    outcome = CreateFeature(Provider);
    Check(!outcome.answered && outcome.handle == kSentinel && gUsedLineCalls == 1,
        "the provider create with the key off is forwarded and notes the memory once");
}

void StandInNotLatched(const std::wstring& folder)
{
    if (!LoadGamePair(folder)) return;
    Check(xmx::Latch(false, false) == xmx::Selection::eOff, "XMX stays off");
    Check(xmx::ConfigureStandIn(true), "the stand-in key turns on");
    auto outcome = CreateFeature(Runtime);
    Check(!outcome.answered && outcome.handle == kSentinel, "an unlatched XMX never answers a runtime create");
    outcome = CreateFeature(Provider);
    Check(!outcome.answered && outcome.handle == kSentinel, "an unlatched XMX never answers a provider create");
    Check(gAttachCalls == 0 && xmx::StandInLive() == 0 && gUsedLineCalls == 0,
        "an unlatched XMX loads no runtime, notes no memory and issues nothing");
}

void StandInNoRuntime()
{
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    Check(xmx::ConfigureStandIn(true), "the stand-in key turns on");
    const auto outcome = CreateFeature(Runtime);
    Check(!outcome.answered && outcome.handle == kSentinel, "with no runtime the create is forwarded to NVIDIA");
    Check(gAttachCalls == 0 && xmx::StandInLive() == 0 && gUsedLineCalls == 0,
        "with no runtime nothing is issued and no memory is noted");
}

void StandInSession(const std::wstring& folder, void (*body)())
{
    if (!LoadGamePair(folder)) return;
    Check(xmx::Latch(true, false) == xmx::Selection::eOn, "XMX latches on");
    body();
}

void StandInCreate()
{
    Check(!xmx::ConfigureStandIn(false), "the key starts off, so turning it off changes nothing");
    Check(xmx::ConfigureStandIn(true), "turning the key on reports the change");
    Check(!xmx::ConfigureStandIn(true), "turning it on again reports no change");
    auto outcome = CreateFeature(Provider);
    Check(!outcome.answered && outcome.handle == kSentinel && xmx::StandInLive() == 0,
        "the provider layer never stands in, even with the key on");
    Check(gAttachCalls == 1, "the provider layer prepares the runtime for NVIDIA's create");
    Check(gUsedLineCalls == 1, "the provider layer notes the memory once for NVIDIA's create");
    outcome = CreateFeature(Runtime, NVSDK_NGX_Feature_SuperSampling);
    Check(!outcome.answered && outcome.handle == kSentinel && xmx::StandInLive() == 0,
        "the stand-in answers only the frame generation feature");
    outcome = CreateFeature(Runtime);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success, "a latched, ready XMX answers the runtime create");
    Check(outcome.handle != kSentinel && IdOf(outcome.handle) == 2000001, "the answer carries the first stand-in handle");
    Check(xmx::StandInLive() == 1, "one handle is live");
    Check(gUsedLineCalls == 2, "an answered runtime create notes the memory");
    outcome = CreateFeature(Provider);
    Check(!outcome.answered && xmx::StandInLive() == 1, "the provider layer still answers nothing with a handle live");
    outcome = CreateFeature(Runtime, NVSDK_NGX_Feature_FrameGeneration, nullptr);
    Check(outcome.answered && outcome.handle != kSentinel && xmx::StandInLive() == 2,
        "a runtime create with no command list is still answered");
    Check(ReleaseFeature(Runtime, outcome.handle).answered && xmx::StandInLive() == 1,
        "the handle made with no command list is released");
    NVSDK_NGX_Result bare = NVSDK_NGX_Result_Fail;
    Check(!xmx::AnswerNgxCall(Runtime, xmx::ProviderCall::eCreate,
              {ListPointer(), NVSDK_NGX_Feature_FrameGeneration, nullptr, nullptr}, bare)
            && xmx::StandInLive() == 1,
        "a runtime create with nowhere to write the handle is not answered and issues nothing");
}

void StandInRouteFailed()
{
    xmx::NoteProviderRoute(false);
    Check(xmx::ConfigureStandIn(true), "the stand-in key turns on");
    Check(xmx::ReadStatus(false).failure == "the hook on NVIDIA's generator could not be installed",
        "with no stand-in serving, the missing provider hook is reported");
    const auto created = CreateFeature(Runtime);
    if (!Check(created.answered && xmx::StandInLive() == 1, "a stand-in handle is issued")) return;
    Check(xmx::ReadStatus(false).failure.empty(), "a stand-in serving needs no provider hook, so none is reported");
    Check(ReleaseFeature(Runtime, created.handle).answered && xmx::StandInLive() == 0, "the handle is released");
    Check(xmx::ReadStatus(false).failure == "the hook on NVIDIA's generator could not be installed",
        "with the stand-in gone, the missing provider hook is reported again");
}

void StandInEvaluate()
{
    xmx::ConfigureStandIn(true);
    const auto created = CreateFeature(Runtime);
    if (!Check(created.answered, "a stand-in handle is issued")) return;
    const void* handle = created.handle;
    const FakeParameters enabled(1), disabled(0), absent(0, false);

    gGeneratorAnswer = false;
    gPassthroughAnswer = true;
    auto outcome = EvaluateFeature(Runtime, handle, &enabled);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && gPassthroughCalls == 1
            && gGeneratorEvaluates == 1,
        "a generator that declines leaves the stand-in to repeat the real frame, never to NVIDIA");

    gPassthroughAnswer = false;
    outcome = EvaluateFeature(Runtime, handle, &enabled);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_FAIL_InvalidParameter && gPassthroughCalls == 2,
        "a passthrough that cannot copy still answers, with an invalid parameter");

    gGeneratorAnswer = true;
    gPassthroughAnswer = true;
    outcome = EvaluateFeature(Runtime, handle, &enabled);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && gGeneratorEvaluates == 3
            && gPassthroughCalls == 2,
        "a generator that answers is the answer, with no passthrough");

    outcome = EvaluateFeature(Runtime, handle, &absent);
    Check(outcome.answered && gGeneratorEvaluates == 4 && gPassthroughCalls == 2,
        "a missing EnableInterp counts as interpolating");

    outcome = EvaluateFeature(Runtime, handle, &disabled);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && gPassthroughCalls == 3
            && gGeneratorEvaluates == 4,
        "EnableInterp 0 on a stand-in handle passes the real frame through without asking the generator");

    gPassthroughAnswer = false;
    outcome = EvaluateFeature(Runtime, handle, nullptr);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_FAIL_InvalidParameter && gPassthroughCalls == 4
            && gGeneratorEvaluates == 4,
        "no parameters on a stand-in handle is answered, with an invalid parameter, and never reaches the generator");
    gPassthroughAnswer = true;

    outcome = EvaluateFeature(Runtime, handle, &enabled, nullptr);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && gGeneratorEvaluates == 5,
        "a stand-in handle evaluated with no command list is still answered");

    const unsigned passthroughs = gPassthroughCalls, evaluates = gGeneratorEvaluates;
    gGeneratorAnswer = true;
    outcome = EvaluateFeature(Runtime, kForeign, &enabled);
    Check(!outcome.answered && gGeneratorEvaluates == evaluates && gPassthroughCalls == passthroughs,
        "a foreign handle at the runtime layer is left alone, the generator is not asked");
    outcome = EvaluateFeature(Runtime, kForeign, &disabled);
    Check(!outcome.answered && gGeneratorEvaluates == evaluates && gPassthroughCalls == passthroughs,
        "EnableInterp 0 on a foreign handle at the runtime layer is left alone too");

    outcome = EvaluateFeature(Provider, kForeign, &disabled);
    Check(outcome.answered && gGeneratorEvaluates == evaluates + 1 && gPassthroughCalls == passthroughs,
        "EnableInterp 0 on a foreign handle at the provider layer still reaches the generator");
    gGeneratorAnswer = false;
    outcome = EvaluateFeature(Provider, kForeign, &enabled);
    Check(!outcome.answered && gPassthroughCalls == passthroughs && gGeneratorEvaluates == evaluates + 2,
        "a foreign handle the generator declines at the provider layer is forwarded, never passed through");
    gGeneratorAnswer = true;

    outcome = EvaluateFeature(Provider, handle, &disabled);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && gPassthroughCalls == passthroughs + 1
            && gGeneratorEvaluates == evaluates + 2,
        "a stand-in handle that reaches the provider layer is answered, never forwarded");

    xmx::ConfigureStandIn(false);
    const unsigned served = gPassthroughCalls;
    outcome = EvaluateFeature(Runtime, handle, &disabled);
    Check(outcome.answered && gPassthroughCalls == served + 1, "an issued handle keeps being served with the key off");
    outcome = CreateFeature(Runtime);
    Check(!outcome.answered && outcome.handle == kSentinel, "a runtime create after the key turned off goes to NVIDIA");

    gTranslatedGpu = true;
    Check(!xmx::Latched(), "an RTX 30 or RTX 20 GPU unlatches XMX");
    gGeneratorAnswer = true;
    outcome = EvaluateFeature(Runtime, handle, &disabled);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && gPassthroughCalls == served + 2,
        "an unlatched XMX still serves an issued handle, with the real frame");
    outcome = ReleaseFeature(Runtime, handle);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && xmx::StandInLive() == 0,
        "an unlatched XMX with the key off still releases an issued handle");
}

void StandInRelease()
{
    xmx::ConfigureStandIn(true);
    const auto created = CreateFeature(Runtime);
    if (!Check(created.answered, "a stand-in handle is issued")) return;
    const void* handle = created.handle;
    const FakeParameters disabled(0);
    Check(xmx::StandInLive() == 1, "one handle is live");

    auto outcome = ReleaseFeature(Runtime, kForeign);
    Check(!outcome.answered && xmx::StandInLive() == 1, "a release of a foreign pointer goes to NVIDIA");
    outcome = ReleaseFeature(Runtime, nullptr);
    Check(!outcome.answered, "a release of nothing goes to NVIDIA");
    outcome = ReleaseFeature(Provider, handle);
    Check(!outcome.answered && xmx::StandInLive() == 1, "the provider layer never releases, even a stand-in handle");
    outcome = ReleaseFeature(Provider, kForeign);
    Check(!outcome.answered, "the provider layer leaves a foreign release alone");
    outcome = ReleaseFeature(Runtime, handle);
    Check(outcome.answered && outcome.result == NVSDK_NGX_Result_Success && xmx::StandInLive() == 0,
        "a runtime release of a stand-in handle is answered and retires it");
    outcome = ReleaseFeature(Runtime, handle);
    Check(!outcome.answered, "a released handle is foreign afterwards");

    const unsigned passthroughs = gPassthroughCalls, evaluates = gGeneratorEvaluates;
    outcome = EvaluateFeature(Runtime, handle, &disabled);
    Check(!outcome.answered && gGeneratorEvaluates == evaluates && gPassthroughCalls == passthroughs,
        "a runtime evaluate on a released handle follows the foreign rules");
    outcome = EvaluateFeature(Provider, handle, &disabled);
    Check(outcome.answered && gGeneratorEvaluates == evaluates + 1 && gPassthroughCalls == passthroughs,
        "a provider evaluate on a released handle follows the foreign rules");
}

void StandInLimit()
{
    xmx::ConfigureStandIn(true);
    NVSDK_NGX_Handle* issued[16]{};
    for (auto& handle : issued)
    {
        const auto outcome = CreateFeature(Runtime);
        handle = outcome.handle;
        if (!Check(outcome.answered, "a handle under the limit is issued")) return;
    }
    Check(xmx::StandInLive() == 16, "sixteen handles are live");
    auto outcome = CreateFeature(Runtime);
    Check(!outcome.answered && outcome.handle == kSentinel && xmx::StandInLive() == 16,
        "the seventeenth runtime create is forwarded to NVIDIA");
    outcome = CreateFeature(Runtime);
    Check(!outcome.answered && gLimitLogs == 1, "the limit is logged once per process");
    Check(ReleaseFeature(Runtime, issued[3]).answered, "one handle is released");
    outcome = CreateFeature(Runtime);
    Check(outcome.answered && xmx::StandInLive() == 16, "a release makes room for the next create");
}
}

namespace single_module
{
HMODULE Self() noexcept
{
    return gSelf ? gSelf : GetModuleHandleW(nullptr);
}

void Log(const wchar_t* text) noexcept
{
    gLimitLogs += wcsstr(text, L"stand-in limit") != nullptr;
    std::wprintf(L"log: %ls\n", text);
}
}

namespace gpu_dispatch
{
bool UsesTranslatedProgram() noexcept
{
    return gTranslatedGpu;
}
}

namespace xess_mod
{
bool PublishMultiFrameModel(HMODULE fg, HMODULE ll, std::string&) noexcept
{
    gPublishedFg = fg;
    gPublishedLl = ll;
    return true;
}
}

namespace xmx::generator
{
void Attach(const Pair& pair) noexcept
{
    ++gAttachCalls;
    gAttached = pair;
}

bool Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Parameter*) noexcept
{
    ++gGeneratorEvaluates;
    return gGeneratorAnswer;
}

bool Passthrough(ID3D12GraphicsCommandList*, const NVSDK_NGX_Parameter*) noexcept
{
    ++gPassthroughCalls;
    return gPassthroughAnswer;
}

std::string MemoryLine() noexcept
{
    return {};
}

std::string UsedLine(ID3D12GraphicsCommandList*) noexcept
{
    ++gUsedLineCalls;
    return {};
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argv[1];
    const std::wstring folder = argc > 2 ? argv[2] : L"";
    if (mode == L"route-before-latch") RouteBeforeLatch();
    else if (mode == L"no-route") NoRoute();
    else if (mode == L"routes-without-latch") RoutesWithoutLatch();
    else if (mode == L"hooked-without-latch") HookedWithoutLatch();
    else if (mode == L"evaluate-entry") EvaluateEntry();
    else if (mode == L"translated-gpu") TranslatedGpu();
    else if (mode == L"translated-gpu-first") TranslatedGpuFirst();
    else if (mode == L"standalone-keeps-its-reason") StandaloneKeepsItsReason();
    else if (mode == L"native-gpu") NativeGpuStaysLatched();
    else if (mode == L"loaded-runtime") LoadedRuntime(folder);
    else if (mode == L"folder-runtime") LoadedRuntimeFromFolder(folder);
    else if (mode == L"loaded-not-fg78") LoadedRuntimeNotFg78(folder);
    else if (mode == L"stand-in-key-off") StandInKeyOff(folder);
    else if (mode == L"stand-in-not-latched") StandInNotLatched(folder);
    else if (mode == L"stand-in-no-runtime") StandInNoRuntime();
    else if (mode == L"stand-in-create") StandInSession(folder, StandInCreate);
    else if (mode == L"stand-in-route-failed") StandInSession(folder, StandInRouteFailed);
    else if (mode == L"stand-in-evaluate") StandInSession(folder, StandInEvaluate);
    else if (mode == L"stand-in-release") StandInSession(folder, StandInRelease);
    else if (mode == L"stand-in-limit") StandInSession(folder, StandInLimit);
    else
    {
        std::printf("FAIL unknown mode\n");
        return 1;
    }
    std::printf("XMX_MODULE checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures || gChecks == 0 ? 1 : 0;
}
