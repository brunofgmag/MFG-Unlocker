#pragma once
#include <array>
#include <cstddef>
#include <mutex>
#include "anr_rules.h"

namespace anr
{
class Handles
{
public:
    bool Add(const void* handle) noexcept
    {
        if (handle == nullptr) return false;
        std::lock_guard guard(mutex_);
        if (IndexOf(handle) != kHandleLimit) return true;
        for (auto& slot : slots_)
        {
            if (slot != nullptr) continue;
            slot = handle;
            return true;
        }
        return false;
    }

    bool Owns(const void* handle) const noexcept
    {
        std::lock_guard guard(mutex_);
        return IndexOf(handle) != kHandleLimit;
    }

    bool Remove(const void* handle) noexcept
    {
        std::lock_guard guard(mutex_);
        const size_t index = IndexOf(handle);
        if (index == kHandleLimit) return false;
        slots_[index] = nullptr;
        return true;
    }

    size_t Live() const noexcept
    {
        std::lock_guard guard(mutex_);
        size_t live = 0;
        for (const void* slot : slots_) live += slot != nullptr;
        return live;
    }

private:
    size_t IndexOf(const void* handle) const noexcept
    {
        if (handle == nullptr) return kHandleLimit;
        for (size_t index = 0; index < kHandleLimit; ++index)
            if (slots_[index] == handle) return index;
        return kHandleLimit;
    }

    mutable std::mutex mutex_;
    std::array<const void*, kHandleLimit> slots_{};
};
}
