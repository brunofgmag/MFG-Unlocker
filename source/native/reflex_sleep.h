#pragma once
#include <atomic>

namespace reflex_sleep
{
enum class Route { Xell, XmxXell, Skip, Driver };

constexpr Route Choose(bool xellReplaces, bool xmxXell, bool skip) noexcept
{
    if (xellReplaces) return Route::Xell;
    if (xmxXell) return Route::XmxXell;
    return skip ? Route::Skip : Route::Driver;
}

inline std::atomic<bool> skip{false};
inline std::atomic<bool> xmxXell{false};

inline bool Configure(bool on) noexcept
{
    return skip.exchange(on, std::memory_order_relaxed) != on;
}

inline bool Skipping() noexcept
{
    return skip.load(std::memory_order_relaxed);
}

inline bool ConfigureXmxXell(bool on) noexcept
{
    return xmxXell.exchange(on, std::memory_order_relaxed) != on;
}

inline bool XmxXell() noexcept
{
    return xmxXell.load(std::memory_order_relaxed);
}
}
