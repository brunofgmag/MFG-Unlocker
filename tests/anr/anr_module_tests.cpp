#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "anr/anr.h"
#include "fake_provider_state.h"
#include "gpu_dispatch.h"
#include "single_module.h"

namespace
{
namespace fs = std::filesystem;

unsigned gChecks = 0, gFailures = 0;
HMODULE gSelf = nullptr;
bool gTranslatedGpu = false;
fs::path gRoot;
fs::path gFakes;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

class FakeDevice : public IUnknown
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    LONG references = 1;
};

class FakeList : public ID3D12DeviceChild
{
public:
    explicit FakeList(FakeDevice* device) : device_(device) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** out) override
    {
        if (device_ == nullptr) return E_FAIL;
        device_->AddRef();
        *out = static_cast<IUnknown*>(device_);
        return S_OK;
    }

private:
    FakeDevice* device_;
};

FakeDevice gDevice;
FakeDevice gSecondDevice;
FakeList gList(&gDevice);
FakeList gSecondList(&gSecondDevice);
FakeList gDetachedList(nullptr);
int gParametersByte = 0;
const void* const kParameters = &gParametersByte;
int gForeignByte = 0;
const void* const kForeign = &gForeignByte;

void* ListPointer(FakeList& list = gList)
{
    return static_cast<ID3D12DeviceChild*>(&list);
}

struct Outcome
{
    bool answered = false;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Handle* handle = nullptr;
};

Outcome Create(void* list = ListPointer(), uintptr_t feature = NVSDK_NGX_Feature_FrameGeneration)
{
    Outcome outcome;
    outcome.answered = anr::AnswerNgxCall(xmx::ProviderCall::eCreate, {list, feature, kParameters, &outcome.handle},
        outcome.result);
    return outcome;
}

Outcome Evaluate(const void* handle, void* list = ListPointer())
{
    Outcome outcome;
    outcome.answered = anr::AnswerNgxCall(xmx::ProviderCall::eEvaluate,
        {list, reinterpret_cast<uintptr_t>(handle), kParameters, nullptr}, outcome.result);
    return outcome;
}

Outcome Release(const void* handle)
{
    Outcome outcome;
    outcome.answered = anr::AnswerNgxCall(xmx::ProviderCall::eRelease,
        {nullptr, reinterpret_cast<uintptr_t>(handle), nullptr, nullptr}, outcome.result);
    return outcome;
}

fs::path ProviderPath()
{
    return gRoot / L"RTXMFG-ANR" / L"nvngx_dlssg.dll";
}

fs::path CopyPath()
{
    return gRoot / L"RTXMFG-ANR" / L"anr_fg_provider.dll";
}

bool SameFile(const fs::path& a, const fs::path& b)
{
    std::ifstream first(a, std::ios::binary), second(b, std::ios::binary);
    const std::string left{std::istreambuf_iterator<char>(first), {}};
    const std::string right{std::istreambuf_iterator<char>(second), {}};
    return first.good() == second.good() && !left.empty() && left == right;
}

void PrimeCopy()
{
    fs::copy_file(ProviderPath(), CopyPath(), fs::copy_options::overwrite_existing);
    LoadLibraryW(CopyPath().c_str());
}

void Prepare(const wchar_t* mode, const wchar_t* fake)
{
    gRoot = fs::temp_directory_path() / (std::wstring(L"anr-module-") + mode + L"-"
        + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(gRoot);
    fs::create_directories(gRoot);
    fs::copy_file(gFakes / L"FakeAnrProvider.dll", gRoot / L"anchor.dll");
    gSelf = LoadLibraryW((gRoot / L"anchor.dll").c_str());
    if (fake == nullptr) return;
    fs::create_directories(gRoot / L"RTXMFG-ANR");
    fs::copy_file(gFakes / fake, ProviderPath());
}

FakeProviderState* Provider()
{
    HMODULE module = GetModuleHandleW(CopyPath().c_str());
    if (module == nullptr) return nullptr;
    auto state = reinterpret_cast<FakeProviderFn>(GetProcAddress(module, "FakeProvider"));
    return state ? state() : nullptr;
}

bool Contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

void NotRequested()
{
    Prepare(L"not-requested", L"FakeAnrProvider.dll");
    Check(anr::Latch(false, false, false) == anr::Selection::eOff, "no anr key latches off");
    Check(!Create().answered, "NVIDIA's generator takes the create while ANR FG is off");
    Check(Provider() == nullptr, "the provider is never loaded while ANR FG is off");
    Check(!anr::ReadStatus().latched && anr::ReadStatus().refusal.empty(), "the status is off with no refusal");
}

void RefusedByXmx()
{
    Prepare(L"refused-xmx", L"FakeAnrProvider.dll");
    Check(anr::Latch(true, true, false) == anr::Selection::eRefusedXmx, "XMX on refuses ANR FG at the latch");
    Check(!Create().answered && Provider() == nullptr, "a refused ANR FG never loads its provider");
    Check(anr::ReadStatus().refusal == "xmx", "the status names XMX as the refusal");
}

void TranslatedGpu()
{
    Prepare(L"translated-gpu", L"FakeAnrProvider.dll");
    gTranslatedGpu = true;
    Check(anr::Latch(true, false, false) == anr::Selection::eRefusedTranslatedGpu,
        "a translated GPU refuses ANR FG at the latch");
    Check(!Create().answered && Provider() == nullptr, "the translated GPU's create goes to NVIDIA's route");
    Check(anr::ReadStatus().refusal == "translated_gpu", "the status names the translated GPU");
}

void TranslatedGpuLater()
{
    Prepare(L"translated-gpu-later", L"FakeAnrProvider.dll");
    Check(anr::Latch(true, false, false) == anr::Selection::eOn, "ANR FG latches before the GPU is known");
    gTranslatedGpu = true;
    Check(!anr::Latched() && anr::ReadStatus().refusal == "translated_gpu",
        "a GPU found translated later refuses ANR FG");
    Check(!Create().answered && Provider() == nullptr, "after the late refusal NVIDIA's route takes the create");
}

void NoProvider()
{
    Prepare(L"no-provider", nullptr);
    anr::Latch(true, false, false);
    Check(!Create().answered, "without RTXMFG-ANR NVIDIA's generator takes the create");
    const auto status = anr::ReadStatus();
    Check(status.latched && !status.loaded, "ANR FG stays latched with no provider loaded");
    Check(Contains(status.failure, "was not found") && Contains(status.failure, "RTXMFG-ANR"),
        "the failure says where the provider was looked for");
}

void RefusedProvider(const wchar_t* mode, const wchar_t* fake, const char* reason)
{
    Prepare(mode, fake);
    anr::Latch(true, false, false);
    Check(!Create().answered, "NVIDIA's generator takes the create from a refused provider");
    Check(Provider() == nullptr, "the refused provider is unloaded");
    const auto status = anr::ReadStatus();
    Check(!status.loaded && Contains(status.failure, reason), "the failure names what the provider lacks");
}

void Latched(const wchar_t* mode)
{
    Prepare(mode, L"FakeAnrProvider.dll");
    anr::Latch(true, false, false);
}

void CreateForwards()
{
    Latched(L"create");
    const Outcome first = Create();
    FakeProviderState* state = Provider();
    if (!Check(state != nullptr, "the provider is loaded from RTXMFG-ANR at the first create")) return;
    Check(first.answered && first.result == NVSDK_NGX_Result_Success, "ANR FG answers the frame generation create");
    Check(first.handle != nullptr && first.handle == state->lastCreated, "the game gets ANR FG's own handle");
    Check(state->creates == 1 && state->lastFeature == NVSDK_NGX_Feature_FrameGeneration,
        "the create reaches ANR FG as frame generation");
    Check(state->lastList == ListPointer() && state->lastParameters == kParameters,
        "the game's command list and parameters reach ANR FG unchanged");
    Check(state->inits == 1 && state->lastDevice == static_cast<void*>(static_cast<IUnknown*>(&gDevice)),
        "ANR FG is initialized on the command list's device");
    Check(fs::path(state->dataPath) == ProviderPath().parent_path(), "ANR FG's data path is its own folder");
    Check(SameFile(ProviderPath(), CopyPath()), "ANR FG is loaded from a byte-identical copy beside its DLL");
    Check(GetModuleHandleW(ProviderPath().c_str()) == nullptr,
        "the DLL under NVIDIA's file name is never loaded, so the loader cannot confuse it with NVIDIA's");
    Check(state->applicationId == anr::kApplicationId, "ANR FG gets the fixed application id");
    Check(gDevice.references == 1, "the device's reference from GetDevice is returned");
    const Outcome second = Create();
    Check(second.answered && second.handle != first.handle && state->inits == 1,
        "a second create on the same device initializes nothing again");
    const auto status = anr::ReadStatus();
    Check(status.loaded && status.creates == 2 && status.live == 2 && status.failure.empty(),
        "the status counts two live creates and no failure");
    Check(Contains(status.runtime, "RTXMFG-ANR"), "the status names the provider's path");
}

void NvidiaLoadedFirst()
{
    Latched(L"nvidia-loaded-first");
    const fs::path nvidiaFolder = gRoot / L"nvidia";
    fs::create_directories(nvidiaFolder);
    fs::copy_file(gFakes / L"FakeNvidiaProvider.dll", nvidiaFolder / L"nvngx_dlssg.dll");
    HMODULE nvidia = LoadLibraryW((nvidiaFolder / L"nvngx_dlssg.dll").c_str());
    if (!Check(nvidia != nullptr, "NVIDIA's provider is loaded first under the same file name")) return;
    const Outcome created = Create();
    Check(created.answered, "ANR FG answers the create with NVIDIA's provider already loaded");
    Check(Provider() != nullptr && Provider()->creates == 1, "the create reaches ANR FG's own module");
    Check(anr::ReadStatus().failure.empty(), "no failure while NVIDIA's provider is loaded");
}

void StaleCopy()
{
    Latched(L"stale-copy");
    fs::copy_file(gFakes / L"FakeNvidiaProvider.dll", CopyPath());
    const Outcome created = Create();
    Check(created.answered, "ANR FG answers the create over a stale copy");
    Check(SameFile(ProviderPath(), CopyPath()), "a copy that differs from ANR FG's DLL is replaced");
    Check(anr::ReadStatus().failure.empty(), "replacing a stale copy is not a failure");
}

void OtherFeature()
{
    Latched(L"other-feature");
    Check(!Create(ListPointer(), NVSDK_NGX_Feature_SuperSampling).answered, "a DLSS create is never answered");
    FakeProviderState* state = Provider();
    Check(state == nullptr || state->creates == 0, "ANR FG never sees another feature's create");
}

void SecondDevice()
{
    Latched(L"second-device");
    Create();
    const Outcome other = Create(ListPointer(gSecondList));
    FakeProviderState* state = Provider();
    Check(other.answered && state != nullptr && state->inits == 2, "a second device initializes ANR FG on it");
    Check(state != nullptr && state->lastDevice == static_cast<void*>(static_cast<IUnknown*>(&gSecondDevice)),
        "the second init gets the second device");
}

void DetachedList()
{
    Latched(L"detached-list");
    Check(!Create(ListPointer(gDetachedList)).answered, "a command list without a device goes to NVIDIA's route");
    Check(Contains(anr::ReadStatus().failure, "no D3D12 device"), "the failure says the list had no device");
}

void InitFails()
{
    Latched(L"init-fails");
    PrimeCopy();
    FakeProviderState* state = Provider();
    if (!Check(state != nullptr, "the provider can be primed")) return;
    state->initResult = NVSDK_NGX_Result_FAIL_PlatformError;
    Check(!Create().answered && state->creates == 0, "a failed init sends the create to NVIDIA's route");
    Check(Contains(anr::ReadStatus().failure, "Init_Ext returned 0xBAD00002"), "the failure names Init_Ext's result");
    state->initResult = NVSDK_NGX_Result_Success;
    Check(Create().answered && state->inits == 2, "a later create retries the init");
    Check(anr::ReadStatus().failure.empty(), "a create that succeeds clears the failure");
}

void CreateFails()
{
    Latched(L"create-fails");
    PrimeCopy();
    FakeProviderState* state = Provider();
    if (!Check(state != nullptr, "the provider can be primed")) return;
    state->createResult = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
    const Outcome outcome = Create();
    Check(!outcome.answered && state->creates == 1, "a failed ANR FG create sends the create to NVIDIA's route");
    Check(anr::ReadStatus().live == 0, "a failed create tracks no handle");
    Check(Contains(anr::ReadStatus().failure, "CreateFeature returned 0xBAD0000D"),
        "the failure names CreateFeature's result");
}

void EvaluateForwards()
{
    Latched(L"evaluate");
    const Outcome created = Create();
    FakeProviderState* state = Provider();
    if (!Check(state != nullptr && created.answered, "a feature is created")) return;
    const Outcome evaluated = Evaluate(created.handle);
    Check(evaluated.answered && evaluated.result == NVSDK_NGX_Result_Success, "ANR FG answers its own evaluate");
    Check(state->evaluates == 1 && state->lastEvaluated == created.handle && state->lastParameters == kParameters
            && state->lastList == ListPointer(), "the evaluate reaches ANR FG with the game's list and parameters");
    Check(!Evaluate(kForeign).answered && state->evaluates == 1, "NVIDIA's handles go to NVIDIA's route");
    state->evaluateResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    const Outcome failed = Evaluate(created.handle);
    Check(failed.answered && failed.result == NVSDK_NGX_Result_FAIL_InvalidParameter,
        "ANR FG's evaluate error goes back to the game");
    const auto status = anr::ReadStatus();
    Check(status.evaluates == 2 && status.failedEvaluates == 1 && status.evaluating,
        "the status counts evaluates and the failed one");
}

void ReleaseForwards()
{
    Latched(L"release");
    const Outcome created = Create();
    FakeProviderState* state = Provider();
    if (!Check(state != nullptr && created.answered, "a feature is created")) return;
    const Outcome released = Release(created.handle);
    Check(released.answered && released.result == NVSDK_NGX_Result_Success && state->releases == 1
            && state->lastReleased == created.handle, "ANR FG releases its own feature");
    Check(!Release(created.handle).answered && state->releases == 1, "a released handle is not released again");
    Check(!Evaluate(created.handle).answered, "a released handle's evaluate goes to NVIDIA's route");
    Check(!Release(kForeign).answered, "NVIDIA's handle's release goes to NVIDIA's route");
    Check(anr::ReadStatus().live == 0, "no feature is live after the release");
}

void Limit()
{
    Latched(L"limit");
    NVSDK_NGX_Handle* first = nullptr;
    unsigned answered = 0;
    for (size_t index = 0; index < anr::kHandleLimit; ++index)
    {
        const Outcome outcome = Create();
        answered += outcome.answered;
        if (index == 0) first = outcome.handle;
    }
    FakeProviderState* state = Provider();
    if (!Check(state != nullptr && answered == anr::kHandleLimit, "ANR FG answers up to the limit")) return;
    const Outcome beyond = Create();
    Check(!beyond.answered && state->releases == 1 && state->lastReleased == state->lastCreated,
        "a create beyond the limit is released in ANR FG and goes to NVIDIA's route");
    Check(Contains(anr::ReadStatus().failure, "limit of 16"), "the failure names the limit");
    Release(first);
    Check(Create().answered, "a release makes room for the next create");
}
}

namespace single_module
{
HMODULE Self() noexcept
{
    return gSelf;
}

void Log(const wchar_t* text) noexcept
{
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

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argv[1];
    gFakes = argv[2];
    if (mode == L"not-requested") NotRequested();
    else if (mode == L"refused-xmx") RefusedByXmx();
    else if (mode == L"translated-gpu") TranslatedGpu();
    else if (mode == L"translated-gpu-later") TranslatedGpuLater();
    else if (mode == L"no-provider") NoProvider();
    else if (mode == L"nvidia-provider")
        RefusedProvider(L"nvidia-provider", L"FakeNvidiaProvider.dll", "NVIDIA's DLSS-G");
    else if (mode == L"no-evaluate")
        RefusedProvider(L"no-evaluate", L"FakeNoEvaluateProvider.dll", "lacks NGX's D3D12");
    else if (mode == L"create") CreateForwards();
    else if (mode == L"nvidia-loaded-first") NvidiaLoadedFirst();
    else if (mode == L"stale-copy") StaleCopy();
    else if (mode == L"other-feature") OtherFeature();
    else if (mode == L"second-device") SecondDevice();
    else if (mode == L"detached-list") DetachedList();
    else if (mode == L"init-fails") InitFails();
    else if (mode == L"create-fails") CreateFails();
    else if (mode == L"evaluate") EvaluateForwards();
    else if (mode == L"release") ReleaseForwards();
    else if (mode == L"limit") Limit();
    else
    {
        std::printf("FAIL unknown mode\n");
        return 1;
    }
    std::printf("ANR_MODULE checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures || gChecks == 0 ? 1 : 0;
}
