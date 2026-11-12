#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include "window_stats.h"

namespace main_thread_waits
{
inline window_stats::Ring<2048> presentWaits, reflexWaits, frameIntervals;
inline std::atomic<int64_t> lastPresentStart{0};
inline std::atomic<uint64_t> reflexSkips{0}, xellSleeps{0};

inline int64_t Now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

inline int64_t Frequency() noexcept
{
    static const int64_t frequency = []
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : int64_t{1};
    }();
    return frequency;
}

inline uint32_t Microseconds(int64_t from, int64_t to) noexcept
{
    const int64_t ticks = to - from;
    if (ticks <= 0) return 0;
    const int64_t microseconds = ticks / Frequency() * 1000000 + ticks % Frequency() * 1000000 / Frequency();
    return microseconds > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(microseconds);
}

inline void NotePresent(int64_t start, int64_t end) noexcept
{
    presentWaits.Add(Microseconds(start, end));
    const int64_t previous = lastPresentStart.exchange(start, std::memory_order_relaxed);
    if (previous != 0) frameIntervals.Add(Microseconds(previous, start));
}

inline void NoteReflexSleep(int64_t start, int64_t end) noexcept
{
    reflexWaits.Add(Microseconds(start, end));
}

inline void NoteReflexSkip() noexcept
{
    reflexSkips.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteXellSleep() noexcept
{
    xellSleeps.fetch_add(1, std::memory_order_relaxed);
}

inline std::string DrainLine()
{
    const auto present = presentWaits.Drain();
    const auto reflex = reflexWaits.Drain();
    const auto interval = frameIntervals.Drain();
    const uint64_t skipped = reflexSkips.exchange(0, std::memory_order_relaxed);
    const uint64_t xell = xellSleeps.exchange(0, std::memory_order_relaxed);
    if (present.events == 0 && reflex.events == 0 && skipped == 0 && xell == 0) return {};
    return window_stats::WaitsLine(present, reflex, skipped, xell, interval);
}
}
