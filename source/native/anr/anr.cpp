#include "anr.h"
#include "anr_handles.h"
#include "gpu_dispatch.h"
#include "single_module.h"
#include "xess/runtime_search.h"
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace anr
{
namespace
{
using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

constexpr uint32_t kRepeatedLogLines = 32;
constexpr uint64_t kRecentMs = 2000;

struct Provider
{
    HMODULE module = nullptr;
    std::wstring source;
    std::wstring path;
    std::wstring folder;
    InitFn init = nullptr;
    CreateFn create = nullptr;
    EvaluateFn evaluate = nullptr;
    ReleaseFn release = nullptr;
};

std::atomic<int> gSelection{static_cast<int>(Selection::eOff)};
std::once_flag gLatchOnce;
std::once_flag gLoadOnce;
std::atomic<bool> gLoaded{false};
Provider gProvider;
Handles gHandles;
std::mutex gDevicesMutex;
std::vector<const void*> gDevices;
std::mutex gFailureMutex;
std::string gFailure;
std::atomic<uint64_t> gCreates{0};
std::atomic<uint64_t> gEvaluates{0};
std::atomic<uint64_t> gFailedEvaluates{0};
std::atomic<uint64_t> gLastEvaluateTick{0};
std::atomic<uint32_t> gRepeatedLines{0};

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

void Emit(const std::string& text) noexcept
{
    try
    {
        const std::string line = "ANR " + text;
        const int count = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (count <= 0) return;
        std::wstring wide(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), count);
        single_module::Log(wide.c_str());
    }
    catch (...) {}
}

void EmitRepeated(const std::string& text) noexcept
{
    if (gRepeatedLines.fetch_add(1, std::memory_order_relaxed) < kRepeatedLogLines) Emit(text);
}

std::string Hex(NVSDK_NGX_Result result)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
    return text;
}

void Fail(const std::string& reason) noexcept
{
    try
    {
        {
            std::lock_guard guard(gFailureMutex);
            gFailure = reason;
        }
        EmitRepeated("failed: " + reason + "; NVIDIA's generator takes this create");
    }
    catch (...) {}
}

void ClearFailure() noexcept
{
    std::lock_guard guard(gFailureMutex);
    gFailure.clear();
}

Selection Current() noexcept
{
    return static_cast<Selection>(gSelection.load(std::memory_order_acquire));
}

void Announce(Selection selection) noexcept
{
    switch (selection)
    {
    case Selection::eOn:
        Emit("generation selected: ANR FG replaces NVIDIA's generator");
        break;
    case Selection::eRefusedXmx:
        Emit("refused: XMX is on, and only one model replaces NVIDIA's generator");
        break;
    case Selection::eRefusedStandalone:
        Emit("refused: XeMFG standalone is on and replaces DLSS frame generation, which ANR FG needs");
        break;
    case Selection::eRefusedTranslatedGpu:
        Emit("refused: this RTX 30 or RTX 20 GPU runs DLSS frame generation through MFG Unlocker's own program, "
            "which ANR FG does not replace");
        break;
    case Selection::eOff:
        break;
    }
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

void* ExportAddress(HMODULE module, const char* name) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return nullptr;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (!directory.VirtualAddress || directory.VirtualAddress >= imageSize
            || directory.Size > imageSize - directory.VirtualAddress) return nullptr;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        if (exports->AddressOfNames >= imageSize || exports->AddressOfNameOrdinals >= imageSize
            || exports->AddressOfFunctions >= imageSize
            || static_cast<uint64_t>(exports->NumberOfNames) * 4 > imageSize - exports->AddressOfNames
            || static_cast<uint64_t>(exports->NumberOfNames) * 2 > imageSize - exports->AddressOfNameOrdinals
            || static_cast<uint64_t>(exports->NumberOfFunctions) * 4 > imageSize - exports->AddressOfFunctions)
            return nullptr;
        const auto* names = reinterpret_cast<const uint32_t*>(base + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const uint16_t*>(base + exports->AddressOfNameOrdinals);
        const auto* functions = reinterpret_cast<const uint32_t*>(base + exports->AddressOfFunctions);
        const size_t length = strlen(name);
        for (uint32_t i = 0; i != exports->NumberOfNames; ++i)
        {
            if (names[i] >= imageSize || length + 1 > imageSize - names[i]) continue;
            if (memcmp(base + names[i], name, length + 1) != 0) continue;
            if (ordinals[i] >= exports->NumberOfFunctions) return nullptr;
            const uint32_t target = functions[ordinals[i]];
            const bool forwarded = target >= directory.VirtualAddress && target - directory.VirtualAddress < directory.Size;
            return target == 0 || target >= imageSize || forwarded ? nullptr : const_cast<uint8_t*>(base + target);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

Exports ReadExports(HMODULE module) noexcept
{
    Exports exports;
    exports.identity = ExportAddress(module, kIdentityExport) != nullptr;
    exports.init = ExportAddress(module, kInitExport) != nullptr;
    exports.create = ExportAddress(module, kCreateExport) != nullptr;
    exports.evaluate = ExportAddress(module, kEvaluateExport) != nullptr;
    exports.release = ExportAddress(module, kReleaseExport) != nullptr;
    exports.nvidiaProvider = ExportAddress(module, kNvidiaProviderExport) != nullptr;
    return exports;
}

std::string OwnerOf(const void* address)
{
    HMODULE owner = nullptr;
    if (address == nullptr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &owner))
        return "nothing";
    return Utf8(xess_runtime_search::ModulePath(owner));
}

std::string ResolverLine(HMODULE module)
{
    return "GetProcAddress finds " + std::string(kNvidiaProviderExport) + " in "
        + OwnerOf(reinterpret_cast<const void*>(GetProcAddress(module, kNvidiaProviderExport))) + " and "
        + kIdentityExport + " in "
        + OwnerOf(reinterpret_cast<const void*>(GetProcAddress(module, kIdentityExport)));
}

std::wstring FoundCandidate(std::wstring& searched)
{
    const auto bases = xess_runtime_search::ReadBases(xess_runtime_search::ModulePath(single_module::Self()),
        xess_runtime_search::ModulePath(nullptr));
    for (const auto& candidate : Candidates(bases.module, bases.game))
    {
        searched += (searched.empty() ? L"" : L", ") + candidate.wstring();
        if (xess_runtime_search::RegularFile(candidate)) return candidate.wstring();
    }
    return {};
}

bool SameBytes(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(b, error) || std::filesystem::file_size(a, error)
            != std::filesystem::file_size(b, error) || error)
        return false;
    std::ifstream first(a, std::ios::binary), second(b, std::ios::binary);
    if (!first || !second) return false;
    return std::equal(std::istreambuf_iterator<char>(first), std::istreambuf_iterator<char>(),
        std::istreambuf_iterator<char>(second));
}

std::wstring PreparedCopy(const std::wstring& source)
{
    const auto copy = LoadedCopy(source);
    if (SameBytes(source, copy)) return copy.wstring();
    if (!CopyFileW(source.c_str(), copy.c_str(), FALSE))
    {
        Fail(Utf8(source) + " could not be copied to " + Utf8(copy.wstring()) + " (error "
            + std::to_string(GetLastError()) + ")");
        return {};
    }
    return copy.wstring();
}

bool Bind(HMODULE module, const std::wstring& path)
{
    const std::wstring loaded = xess_runtime_search::ModulePath(module);
    if (_wcsicmp(loaded.c_str(), path.c_str()) != 0)
    {
        Fail("loading " + Utf8(path) + " returned the module " + Utf8(loaded) + "; " + ResolverLine(module));
        return false;
    }
    const Exports exports = ReadExports(module);
    if (const char* refusal = ProviderRefusal(exports))
    {
        Fail(Utf8(path) + " is not used: " + refusal + "; " + ResolverLine(module));
        return false;
    }
    gProvider.module = module;
    gProvider.path = path;
    gProvider.folder = std::filesystem::path(path).parent_path().wstring();
    gProvider.init = reinterpret_cast<InitFn>(ExportAddress(module, kInitExport));
    gProvider.create = reinterpret_cast<CreateFn>(ExportAddress(module, kCreateExport));
    gProvider.evaluate = reinterpret_cast<EvaluateFn>(ExportAddress(module, kEvaluateExport));
    gProvider.release = reinterpret_cast<ReleaseFn>(ExportAddress(module, kReleaseExport));
    return true;
}

void LoadProvider() noexcept
{
    std::call_once(gLoadOnce, []
    {
        try
        {
            std::wstring searched;
            const std::wstring path = FoundCandidate(searched);
            if (path.empty())
            {
                Fail("nvngx_dlssg.dll of ANR FG was not found in " + Utf8(searched));
                return;
            }
            const std::wstring copy = PreparedCopy(path);
            if (copy.empty()) return;
            HMODULE module = LoadLibraryExW(copy.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (module == nullptr)
            {
                Fail(Utf8(copy) + " could not be loaded (error " + std::to_string(GetLastError()) + ")");
                return;
            }
            if (!Bind(module, copy))
            {
                FreeLibrary(module);
                return;
            }
            gProvider.source = path;
            gLoaded.store(true, std::memory_order_release);
            Emit("provider " + Utf8(path) + ", loaded as " + Utf8(copy));
        }
        catch (...)
        {
            Fail("the ANR FG provider could not be prepared");
        }
    });
}

bool ProviderReady() noexcept
{
    LoadProvider();
    return gLoaded.load(std::memory_order_acquire);
}

const void* ListDevice(void* commandList) noexcept
{
    auto* list = static_cast<ID3D12GraphicsCommandList*>(commandList);
    ID3D12Device* device = nullptr;
    if (list == nullptr || FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
        || device == nullptr)
        return nullptr;
    device->Release();
    return device;
}

bool Initialized(const void* device) noexcept
{
    std::lock_guard guard(gDevicesMutex);
    if (std::find(gDevices.begin(), gDevices.end(), device) != gDevices.end()) return true;
    const NVSDK_NGX_Result result = gProvider.init(kApplicationId, gProvider.folder.c_str(),
        static_cast<ID3D12Device*>(const_cast<void*>(device)), NVSDK_NGX_Version_API, nullptr);
    if (NVSDK_NGX_FAILED(result))
    {
        Fail("NVSDK_NGX_D3D12_Init_Ext returned " + Hex(result));
        return false;
    }
    try
    {
        gDevices.push_back(device);
    }
    catch (...) {}
    return true;
}

bool AnswerCreate(const xmx::ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    if (args.featureOrHandle != NVSDK_NGX_Feature_FrameGeneration || !Latched() || args.outHandle == nullptr)
        return false;
    if (!ProviderReady()) return false;
    const void* device = ListDevice(args.commandList);
    if (device == nullptr)
    {
        Fail("the create's command list has no D3D12 device");
        return false;
    }
    if (!Initialized(device)) return false;
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result created = gProvider.create(static_cast<ID3D12GraphicsCommandList*>(args.commandList),
        NVSDK_NGX_Feature_FrameGeneration,
        static_cast<NVSDK_NGX_Parameter*>(const_cast<void*>(args.parameters)), &handle);
    if (NVSDK_NGX_FAILED(created) || handle == nullptr)
    {
        Fail("NVSDK_NGX_D3D12_CreateFeature returned " + Hex(created));
        return false;
    }
    if (!gHandles.Add(handle))
    {
        gProvider.release(handle);
        Fail("the limit of " + std::to_string(kHandleLimit) + " live ANR FG features was reached");
        return false;
    }
    ClearFailure();
    gCreates.fetch_add(1, std::memory_order_relaxed);
    *static_cast<NVSDK_NGX_Handle**>(args.outHandle) = handle;
    result = created;
    EmitRepeated("frame generation created by ANR FG, live " + std::to_string(gHandles.Live()));
    return true;
}

bool AnswerEvaluate(const xmx::ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(args.featureOrHandle);
    if (!gHandles.Owns(handle)) return false;
    gLastEvaluateTick.store(GetTickCount64(), std::memory_order_relaxed);
    gEvaluates.fetch_add(1, std::memory_order_relaxed);
    result = gProvider.evaluate(static_cast<ID3D12GraphicsCommandList*>(args.commandList), handle,
        static_cast<NVSDK_NGX_Parameter*>(const_cast<void*>(args.parameters)), nullptr);
    if (NVSDK_NGX_FAILED(result))
    {
        gFailedEvaluates.fetch_add(1, std::memory_order_relaxed);
        EmitRepeated("NVSDK_NGX_D3D12_EvaluateFeature returned " + Hex(result));
    }
    return true;
}

bool AnswerRelease(const xmx::ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    auto* handle = reinterpret_cast<NVSDK_NGX_Handle*>(args.featureOrHandle);
    if (!gHandles.Remove(handle)) return false;
    result = gProvider.release(handle);
    EmitRepeated("frame generation released, live " + std::to_string(gHandles.Live()));
    return true;
}
}

Selection Latch(bool requested, bool xmxRequested, bool standaloneRequested) noexcept
{
    std::call_once(gLatchOnce, [&]
    {
        const Selection selection = ForGpu(Select(requested, xmxRequested, standaloneRequested),
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

bool AnswerNgxCall(xmx::ProviderCall call, const xmx::ProviderArgs& args, NVSDK_NGX_Result& result) noexcept
{
    try
    {
        switch (call)
        {
        case xmx::ProviderCall::eCreate:
            return AnswerCreate(args, result);
        case xmx::ProviderCall::eEvaluate:
            return AnswerEvaluate(args, result);
        case xmx::ProviderCall::eRelease:
            return AnswerRelease(args, result);
        }
    }
    catch (...) {}
    return false;
}

Status ReadStatus() noexcept
{
    Status out;
    try
    {
        out.latched = Latched();
        out.refusal = RefusalName(Current());
        out.loaded = gLoaded.load(std::memory_order_acquire);
        if (out.loaded) out.runtime = Utf8(gProvider.source);
        const uint64_t evaluated = gLastEvaluateTick.load(std::memory_order_relaxed);
        out.evaluating = evaluated != 0 && GetTickCount64() - evaluated < kRecentMs;
        out.live = gHandles.Live();
        out.creates = gCreates.load(std::memory_order_relaxed);
        out.evaluates = gEvaluates.load(std::memory_order_relaxed);
        out.failedEvaluates = gFailedEvaluates.load(std::memory_order_relaxed);
        std::lock_guard guard(gFailureMutex);
        out.failure = gFailure;
    }
    catch (...) {}
    return out;
}
}
