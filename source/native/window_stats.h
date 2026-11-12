#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace window_stats
{
struct Spread
{
    uint64_t events = 0;
    float median = 0.f, p90 = 0.f, max = 0.f;
};

inline float NearestRank(const std::vector<float>& sorted, double fraction) noexcept
{
    if (sorted.empty()) return 0.f;
    const auto rank = static_cast<size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

inline Spread Summarize(std::vector<float> values, uint64_t events)
{
    std::sort(values.begin(), values.end());
    return {events, NearestRank(values, 0.5), NearestRank(values, 0.9), values.empty() ? 0.f : values.back()};
}

template <size_t Capacity>
class Ring
{
public:
    void Add(uint32_t microseconds) noexcept
    {
        const uint64_t index = written.fetch_add(1, std::memory_order_relaxed);
        slots[index % Capacity].store(std::max<uint32_t>(microseconds, 1), std::memory_order_relaxed);
    }

    Spread Drain()
    {
        const uint64_t end = written.load(std::memory_order_relaxed);
        const uint64_t events = end - drained;
        const uint64_t first = events > Capacity ? end - Capacity : drained;
        std::vector<float> millis;
        millis.reserve(static_cast<size_t>(end - first));
        for (uint64_t index = first; index < end; ++index)
        {
            const uint32_t value = slots[index % Capacity].exchange(0, std::memory_order_relaxed);
            if (value != 0) millis.push_back(static_cast<float>(value) / 1000.f);
        }
        drained = end;
        return Summarize(std::move(millis), events);
    }

private:
    std::array<std::atomic<uint32_t>, Capacity> slots{};
    std::atomic<uint64_t> written{0};
    uint64_t drained = 0;
};

inline std::string WaitsLine(const Spread& present, const Spread& reflex, uint64_t reflexSkipped,
    uint64_t xellSleeps, const Spread& interval)
{
    char text[256]{};
    std::snprintf(text, sizeof(text),
        "Main-thread waits 10 s: present n=%llu med=%.2f p90=%.2f max=%.2f ms; reflexSleep n=%llu skipped=%llu xell=%llu "
        "med=%.2f p90=%.2f ms; frame interval med=%.2f ms",
        static_cast<unsigned long long>(present.events), present.median, present.p90, present.max,
        static_cast<unsigned long long>(reflex.events), static_cast<unsigned long long>(reflexSkipped),
        static_cast<unsigned long long>(xellSleeps),
        reflex.median, reflex.p90, interval.median);
    return text;
}
}
