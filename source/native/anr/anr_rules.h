#pragma once
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <vector>

namespace anr
{
inline constexpr wchar_t kFolder[] = L"RTXMFG-ANR";
inline constexpr wchar_t kProvider[] = L"nvngx_dlssg.dll";
inline constexpr wchar_t kLoadedCopy[] = L"anr_fg_provider.dll";
inline constexpr char kIdentityExport[] = "NRFG_PresentationControl";
inline constexpr char kInitExport[] = "NVSDK_NGX_D3D12_Init_Ext";
inline constexpr char kCreateExport[] = "NVSDK_NGX_D3D12_CreateFeature";
inline constexpr char kEvaluateExport[] = "NVSDK_NGX_D3D12_EvaluateFeature";
inline constexpr char kReleaseExport[] = "NVSDK_NGX_D3D12_ReleaseFeature";
inline constexpr char kNvidiaProviderExport[] = "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl";
inline constexpr unsigned long long kApplicationId = 1;
inline constexpr size_t kHandleLimit = 16;

enum class Selection
{
    eOff,
    eOn,
    eRefusedXmx,
    eRefusedStandalone,
    eRefusedTranslatedGpu,
};

constexpr Selection Select(bool requested, bool xmxRequested, bool standaloneRequested) noexcept
{
    if (!requested) return Selection::eOff;
    if (standaloneRequested) return Selection::eRefusedStandalone;
    return xmxRequested ? Selection::eRefusedXmx : Selection::eOn;
}

constexpr Selection ForGpu(Selection selection, bool translatedProgram) noexcept
{
    return selection == Selection::eOn && translatedProgram ? Selection::eRefusedTranslatedGpu : selection;
}

constexpr const char* RefusalName(Selection selection) noexcept
{
    switch (selection)
    {
    case Selection::eRefusedXmx: return "xmx";
    case Selection::eRefusedStandalone: return "xemfg_standalone";
    case Selection::eRefusedTranslatedGpu: return "translated_gpu";
    default: return "";
    }
}

struct Exports
{
    bool identity = false;
    bool init = false;
    bool create = false;
    bool evaluate = false;
    bool release = false;
    bool nvidiaProvider = false;
};

constexpr const char* ProviderRefusal(const Exports& exports) noexcept
{
    if (exports.nvidiaProvider) return "it is NVIDIA's DLSS-G, not ANR FG";
    if (!exports.identity) return "it lacks ANR FG's NRFG_PresentationControl export";
    if (!exports.init || !exports.create || !exports.evaluate || !exports.release)
        return "it lacks NGX's D3D12 Init_Ext, CreateFeature, EvaluateFeature or ReleaseFeature";
    return nullptr;
}

inline std::filesystem::path LoadedCopy(const std::filesystem::path& provider)
{
    return provider.parent_path() / kLoadedCopy;
}

inline std::vector<std::filesystem::path> Candidates(const std::filesystem::path& moduleFolder,
    const std::filesystem::path& gameFolder)
{
    std::vector<std::filesystem::path> candidates;
    for (const auto& base : {moduleFolder, gameFolder})
    {
        if (base.empty()) continue;
        const auto candidate = base / kFolder / kProvider;
        bool known = false;
        for (const auto& existing : candidates) known = known || _wcsicmp(existing.c_str(), candidate.c_str()) == 0;
        if (!known) candidates.push_back(candidate);
    }
    return candidates;
}
}
