#pragma once
#include <cstdint>
#include <optional>

namespace xmx::low_latency
{
struct Request
{
    bool mode = false;
    bool boost = false;
    uint32_t intervalUs = 0;

    constexpr bool operator==(const Request&) const noexcept = default;
};

constexpr Request kReleased{false, false, 0};

constexpr bool HoldWanted(bool xmxXell, bool latched) noexcept
{
    return xmxXell && latched;
}

constexpr Request Forwarded(Request requested, bool hold) noexcept
{
    return {requested.mode && !hold, requested.boost && !hold, requested.intervalUs};
}

constexpr std::optional<Request> Resync(std::optional<Request> last, bool hold) noexcept
{
    if (last) return Forwarded(*last, hold);
    if (hold) return kReleased;
    return std::nullopt;
}
}
