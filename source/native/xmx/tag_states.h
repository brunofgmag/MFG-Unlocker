#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>

namespace xmx::tag_states
{
struct Entry
{
    const void* resource{};
    uint32_t state{};
};

inline SRWLOCK lock = SRWLOCK_INIT;
inline std::array<Entry, 32> entries{};
inline uint32_t next = 0;

inline Entry* Find(const void* resource) noexcept
{
    Entry* found = nullptr;
    for (auto& entry : entries)
        if (entry.resource == resource) found = &entry;
    return found;
}

inline void Record(const void* resource, uint32_t state) noexcept
{
    if (resource == nullptr) return;
    const bool unknown = state == UINT32_MAX;
    AcquireSRWLockExclusive(&lock);
    Entry* slot = Find(resource);
    if (slot == nullptr && !unknown) slot = &entries[next++ % entries.size()];
    if (slot != nullptr) *slot = unknown ? Entry{} : Entry{resource, state};
    ReleaseSRWLockExclusive(&lock);
}

inline uint32_t StateOf(const void* resource, uint32_t fallback) noexcept
{
    uint32_t state = fallback;
    AcquireSRWLockShared(&lock);
    for (const auto& entry : entries)
        if (entry.resource == resource && resource != nullptr) state = entry.state;
    ReleaseSRWLockShared(&lock);
    return state;
}
}
