#pragma once
#include <array>
#include <cstddef>
#include <string>

namespace control_json
{
inline constexpr std::array<const char*, 21> kCarriedFields{"xessModel", "optiScalerUi", "dlssSrPreset",
    "dlssRrPreset", "xessVulkan", "xessVulkanFrames", "xemfgStandalone", "xmx", "xemfgFrames", "xemfgDynamic",
    "xemfgTargetFps", "xemfgRounding", "xemfgAllow1x", "xemfgTargetAsCap", "xemfgFlipQueue", "latencyPacing",
    "dlssgDefaults", "reflexSleepSkip", "xmxXell", "xmxStandIn", "anr"};

inline bool FindValue(const std::string& content, const char* name, size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const size_t colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string::npos;
}

inline void CarryField(std::string& content, const std::string& saved, const char* name)
{
    size_t offset = 0, existing = 0;
    if (FindValue(content, name, offset) || !FindValue(saved, name, existing)) return;
    size_t end = existing;
    while (end < saved.size() && saved[end] != ',' && saved[end] != '}'
        && saved[end] != '\r' && saved[end] != '\n') ++end;
    const size_t close = content.find_last_not_of(" \t\r\n");
    if (end == existing || close == std::string::npos || content[close] != '}') return;
    content.insert(close, std::string(",\"") + name + "\":" + saved.substr(existing, end - existing));
}

inline void Carry(std::string& content, const std::string& saved)
{
    for (const char* name : kCarriedFields)
        CarryField(content, saved, name);
}
}
