#pragma once
#include <cstdint>
#include <string_view>

namespace xmx
{
struct Rvas
{
    uintptr_t factory, init, execute, contextVtable;
};

inline constexpr std::string_view kSupportedFamily = "fg78";
inline constexpr Rvas kFg78Rvas{0x1853B0, 0x1871C0, 0x185800, 0xF45D48};
inline constexpr uint32_t kMaxFrames = 5;

constexpr const Rvas* RvasFor(std::string_view family) noexcept
{
    return family == kSupportedFamily ? &kFg78Rvas : nullptr;
}

enum class Selection
{
    eOff,
    eOn,
    eRefusedStandalone,
    eRefusedTranslatedGpu,
};

constexpr Selection Select(bool requested, bool standaloneRequested) noexcept
{
    if (!requested) return Selection::eOff;
    return standaloneRequested ? Selection::eRefusedStandalone : Selection::eOn;
}

constexpr Selection ForGpu(Selection selection, bool translatedProgram) noexcept
{
    return selection == Selection::eOn && translatedProgram ? Selection::eRefusedTranslatedGpu : selection;
}

constexpr const char* RefusalName(Selection selection) noexcept
{
    switch (selection)
    {
    case Selection::eRefusedStandalone: return "xemfg_standalone";
    case Selection::eRefusedTranslatedGpu: return "translated_gpu";
    default: return "";
    }
}
}
