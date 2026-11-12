#include "xmx.h"
#include "xmx_generator.h"
#include "xmx_latency.h"
#include "xmx_policy.h"
#include "xmx_stand_in.h"
#include "xmx_status.h"
#include "tag_states.h"
#include "gpu_dispatch.h"
#include "single_module.h"
#include "xess/runtime_layout.h"
#include "xess/runtime_search.h"
#include "xess/xess_mod.h"
#include <sl.h>
#include <wrl/client.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace xmx
{
namespace
{
constexpr uint32_t kLogLines = 48;
constexpr uint32_t kMaxTagsPerCall = 64;
constexpr uint64_t kRecentMs = 2000;

std::atomic<int> gSelection{static_cast<int>(Selection::eOff)};
std::once_flag gLatchOnce;
std::once_flag gRuntimeOnce;
std::atomic<bool> gCreateAwaitingEvaluate{false};
std::atomic<bool> gRuntimeReady{false};
std::atomic<bool> gStandIn{false};
std::atomic<bool> gLimitLogged{false};
stand_in::Registry gStandIns;

void Emit(const std::string& text) noexcept
{
    try
    {
        const std::string line = "XMX " + text;
        const int count = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (count <= 0) return;
        std::wstring wide(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), count);
        single_module::Log(wide.c_str());
    }
    catch (...) {}
}

template <typename Work>
void Quietly(Work&& work) noexcept
{
    try
    {
        work();
    }
    catch (...) {}
}

std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (count <= 0) return {};
    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr, nullptr);
    return utf8;
}

std::string PathOf(HMODULE module)
{
    const auto path = xess_runtime_search::ModulePath(module);
    return path.empty() ? std::string("not loaded") : Utf8(path);
}

Selection Current() noexcept
{
    return static_cast<Selection>(gSelection.load(std::memory_order_acquire));
}

std::optional<std::filesystem::path> RuntimeFolder()
{
    const auto bases = xess_runtime_search::ReadBases(xess_runtime_search::ModulePath(single_module::Self()),
        xess_runtime_search::ModulePath(nullptr));
    const auto search = xess_runtime_search::Find(bases, xess_runtime_search::Order::eD3D12);
    if (search.found != SIZE_MAX) return search.folders[search.found].path;
    status::Fail("libxess_fg.dll and libxell.dll were not found ("
        + (search.reason.empty() ? std::string("not_found") : search.reason) + ")");
    return std::nullopt;
}

const Rvas* SupportedRvas(HMODULE fg)
{
    std::string family;
    try
    {
        family = xess_layout::Get(fg).family;
    }
    catch (const std::exception& error)
    {
        status::Fail(std::string("libxess_fg.dll is not a supported build (") + error.what() + ")");
        return nullptr;
    }
    const Rvas* rvas = RvasFor(family);
    if (rvas == nullptr)
        status::Fail("libxess_fg.dll family " + family + " is not supported, only " + std::string(kSupportedFamily));
    return rvas;
}

uint32_t PublishModel(HMODULE fg, HMODULE ll)
{
    std::string error;
    if (xess_mod::PublishMultiFrameModel(fg, ll, error)) return kMaxFrames;
    Log("multi-frame model refused, 2x only: " + error);
    return 1;
}

bool RuntimeModules(HMODULE& fg, HMODULE& ll)
{
    fg = GetModuleHandleW(xess_runtime_search::kFg);
    ll = GetModuleHandleW(xess_runtime_search::kLl);
    if (fg && ll) return true;
    const auto folder = RuntimeFolder();
    if (!folder) return false;
    if (!ll)
        ll = LoadLibraryExW((*folder / xess_runtime_search::kLl).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!fg && ll)
        fg = LoadLibraryExW((*folder / xess_runtime_search::kFg).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (fg && ll) return true;
    status::Fail("libxess_fg.dll and libxell.dll could not be loaded");
    return false;
}

void LoadRuntime() noexcept
{
    std::call_once(gRuntimeOnce, []
    {
        try
        {
            HMODULE fg = nullptr, ll = nullptr;
            if (!RuntimeModules(fg, ll)) return;
            Log("runtime " + PathOf(fg) + " and " + PathOf(ll));
            latency::Functions functions;
            if (latency::Resolve(ll, functions)) latency::Attach(functions);
            else Log("XeLL exports missing in libxell.dll");
            const Rvas* rvas = SupportedRvas(fg);
            if (rvas == nullptr) return;
            generator::Attach({fg, rvas, PublishModel(fg, ll)});
            gRuntimeReady.store(true, std::memory_order_release);
        }
        catch (...)
        {
            Quietly([] { status::Fail("the XeFG runtime could not be prepared"); });
        }
    });
}

bool RuntimeReady() noexcept
{
    LoadRuntime();
    return gRuntimeReady.load(std::memory_order_acquire);
}

void ObserveListDevice(ID3D12GraphicsCommandList* list) noexcept
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (list == nullptr || FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return;
    latency::ObserveDevice(device.Get());
    low_latency::Observe(device.Get());
}

bool Recent(uint64_t tick, uint64_t now) noexcept
{
    return tick != 0 && now - tick < kRecentMs;
}

void Announce(Selection selection) noexcept
{
    Quietly([selection]
    {
        switch (selection)
        {
        case Selection::eOn:
            Emit("generation selected: Intel's XeFG replaces NVIDIA's generator");
            break;
        case Selection::eRefusedStandalone:
            Emit("refused: XeMFG standalone is on and replaces DLSS frame generation, which XMX needs");
            break;
        case Selection::eRefusedTranslatedGpu:
            Emit("refused: this RTX 30 or RTX 20 GPU runs DLSS frame generation through MFG Unlocker's own program, "
                "which XMX does not replace");
            break;
        case Selection::eOff:
            break;
        }
    });
}

Selection Settled() noexcept
{
    const Selection current = Current();
    const Selection settled = ForGpu(current, gpu_dispatch::UsesTranslatedProgram());
    int expected = static_cast<int>(current);
    if (settled != current && gSelection.compare_exchange_strong(expected, static_cast<int>(settled)))
        Announce(settled);
    return Current();
}

bool StandInInterpolates(const void* parameters) noexcept
{
    if (parameters == nullptr) return false;
    int enabled = 1;
    static_cast<const NVSDK_NGX_Parameter*>(parameters)->Get("DLSSG.EnableInterp", &enabled);
    return enabled != 0;
}

NVSDK_NGX_Result StandInEvaluate(const ProviderArgs& args) noexcept
{
    status::lastEvaluateTick.store(GetTickCount64(), std::memory_order_relaxed);
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
    if (StandInInterpolates(args.parameters) && Evaluate(args.commandList, args.parameters, result)) return result;
    return generator::Passthrough(static_cast<ID3D12GraphicsCommandList*>(args.commandList),
               static_cast<const NVSDK_NGX_Parameter*>(args.parameters))
        ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
}

stand_in::Handle* IssueStandIn() noexcept
{
    stand_in::Handle* handle = gStandIns.Issue();
    if (handle == nullptr && !gLimitLogged.exchange(true))
        Quietly([]
        {
            Emit("stand-in limit of " + std::to_string(stand_in::Registry::kLimit)
                + " live handles reached; NVIDIA creates this one");
        });
    return handle;
}

bool LatchedFrameGenerationCreate(const ProviderArgs& args) noexcept
{
    return args.featureOrHandle == NVSDK_NGX_Feature_FrameGeneration && Latched();
}

bool AnswerRuntimeCreate(const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    if (!LatchedFrameGenerationCreate(args)) return false;
    BeforeCreate();
    if (!gStandIn.load(std::memory_order_relaxed) || !RuntimeReady() || args.outHandle == nullptr) return false;
    stand_in::Handle* handle = IssueStandIn();
    if (handle == nullptr) return false;
    NoteCreateMemory(args.commandList);
    *static_cast<NVSDK_NGX_Handle**>(args.outHandle) = reinterpret_cast<NVSDK_NGX_Handle*>(handle);
    Quietly([handle]
    {
        Emit("stand-in answers frame generation create: handle " + std::to_string(handle->Id)
            + ", NVIDIA's generator is not created");
    });
    result = NVSDK_NGX_Result_Success;
    return true;
}

bool AnswerProviderCreate(const ProviderArgs& args) noexcept
{
    if (!LatchedFrameGenerationCreate(args)) return false;
    BeforeCreate();
    NoteCreateMemory(args.commandList);
    return false;
}

bool AnswerEvaluate(NgxLayer layer, const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    if (gStandIns.Owns(reinterpret_cast<const void*>(args.featureOrHandle)))
    {
        result = StandInEvaluate(args);
        return true;
    }
    return layer == NgxLayer::eProvider && Evaluate(args.commandList, args.parameters, result);
}

bool AnswerRelease(NgxLayer layer, const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    unsigned int id = 0;
    if (layer == NgxLayer::eProvider
        || !gStandIns.Retire(reinterpret_cast<const void*>(args.featureOrHandle), &id)) return false;
    Quietly([id] { Emit("stand-in released handle " + std::to_string(id)); });
    result = NVSDK_NGX_Result_Success;
    return true;
}

bool AnswerCreate(NgxLayer layer, const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    return layer == NgxLayer::eRuntime ? AnswerRuntimeCreate(args, result) : AnswerProviderCreate(args);
}
}

void Log(const std::string& text) noexcept
{
    static std::atomic<uint32_t> lines{0};
    if (lines.fetch_add(1, std::memory_order_relaxed) < kLogLines) Emit(text);
}

void LogSummary(const std::string& text) noexcept
{
    Emit(text);
}

Selection Latch(bool requested, bool standaloneRequested) noexcept
{
    std::call_once(gLatchOnce, [&]
    {
        const Selection selection = ForGpu(Select(requested, standaloneRequested),
            gpu_dispatch::UsesTranslatedProgram());
        gSelection.store(static_cast<int>(selection), std::memory_order_release);
        Announce(selection);
    });
    return Settled();
}

bool Latched() noexcept
{
    return Settled() == Selection::eOn;
}

void NoteProviderRoute(bool installed) noexcept
{
    (installed ? status::hooked : status::hookFailed).store(true, std::memory_order_relaxed);
}

void BeforeCreate() noexcept
{
    if (Latched()) LoadRuntime();
}

void NoteCreateMemory(void* commandList) noexcept
{
    if (!Latched()) return;
    const auto line = generator::UsedLine(static_cast<ID3D12GraphicsCommandList*>(commandList));
    if (line.empty()) return;
    Quietly([&line] { Log("NVIDIA frame generation create, before: " + line); });
    gCreateAwaitingEvaluate.store(true, std::memory_order_relaxed);
}

bool Evaluate(void* commandList, const void* parameters, NVSDK_NGX_Result& result) noexcept
{
    if (!Latched()) return false;
    status::lastEvaluateTick.store(GetTickCount64(), std::memory_order_relaxed);
    if (!RuntimeReady()) return false;
    ObserveListDevice(static_cast<ID3D12GraphicsCommandList*>(commandList));
    if (gCreateAwaitingEvaluate.exchange(false, std::memory_order_relaxed))
        Quietly([commandList]
        {
            Log("NVIDIA frame generation create, first evaluate after: "
                + generator::UsedLine(static_cast<ID3D12GraphicsCommandList*>(commandList)));
        });
    if (!generator::Evaluate(static_cast<ID3D12GraphicsCommandList*>(commandList),
            static_cast<const NVSDK_NGX_Parameter*>(parameters)))
        return false;
    result = NVSDK_NGX_Result_Success;
    return true;
}

bool AnswerNgxCall(NgxLayer layer, ProviderCall call, const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    switch (call)
    {
    case ProviderCall::eCreate:
        return AnswerCreate(layer, args, result);
    case ProviderCall::eEvaluate:
        return AnswerEvaluate(layer, args, result);
    case ProviderCall::eRelease:
        return AnswerRelease(layer, args, result);
    }
    return false;
}

bool ConfigureStandIn(bool on) noexcept
{
    return gStandIn.exchange(on, std::memory_order_relaxed) != on;
}

size_t StandInLive() noexcept
{
    return gStandIns.Live();
}

void RecordTagStates(const sl::ResourceTag* tags, uint32_t count) noexcept
{
    if (tags == nullptr || !Latched()) return;
    for (uint32_t index = 0; index < count && index < kMaxTagsPerCall; ++index)
        if (tags[index].resource != nullptr && tags[index].resource->native != nullptr)
            tag_states::Record(tags[index].resource->native, tags[index].resource->state);
}

Status ReadStatus(bool driverGenerating) noexcept
{
    Status out;
    try
    {
        const uint64_t now = GetTickCount64();
        const uint64_t generated = status::lastGeneratedTick.load(std::memory_order_relaxed);
        const uint64_t evaluated = status::lastEvaluateTick.load(std::memory_order_relaxed);
        const bool hooked = status::hooked.load(std::memory_order_relaxed);
        out.latched = Latched();
        out.refusal = RefusalName(Current());
        out.hooked = hooked;
        const bool hookFailed = status::hookFailed.load(std::memory_order_relaxed);
        out.partiallyHooked = hooked && hookFailed;
        out.generating = Recent(generated, now);
        out.evaluating = Recent(evaluated, now);
        out.bypassed = out.latched && policy::Bypassed(hooked, evaluated != 0, driverGenerating);
        out.framesLimit = status::framesLimit.load(std::memory_order_relaxed);
        out.evaluates = status::evaluates.load(std::memory_order_relaxed);
        out.generated = status::generated.load(std::memory_order_relaxed);
        out.idle = policy::IdleText(static_cast<policy::Idle>(status::idle.load(std::memory_order_relaxed)));
        out.failure = status::Failure();
        if (out.latched && out.failure.empty() && !hooked && hookFailed && StandInLive() == 0)
            out.failure = "the hook on NVIDIA's generator could not be installed";
    }
    catch (...) {}
    return out;
}

std::string MemoryLine() noexcept
{
    return generator::MemoryLine();
}
}
