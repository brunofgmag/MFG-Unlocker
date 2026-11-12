#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace xmx::stand_in
{
inline const wchar_t* ConfigLine(bool configured, bool xmxLatched) noexcept
{
    if (!configured) return L"XMX stand-in off: the next frame generation create goes to NVIDIA's generator";
    return xmxLatched
        ? L"XMX stand-in on: the next frame generation create is answered without NVIDIA's generator"
        : L"XMX stand-in on, but XMX is not latched: the next frame generation create goes to NVIDIA's generator";
}

struct Handle
{
    unsigned int Id;
};

class Registry
{
public:
    static constexpr size_t kLimit = 16;
    static constexpr unsigned int kFirstId = 2000001;

    Handle* Issue() noexcept
    {
        try
        {
            std::lock_guard guard(mutex_);
            for (auto& slot : slots_)
            {
                if (slot != nullptr) continue;
                std::unique_ptr<Handle> owned(new (std::nothrow) Handle{nextId_});
                if (owned == nullptr) return nullptr;
                Handle* const issued = owned.get();
                issued_.push_back(std::move(owned));
                ++nextId_;
                slot = issued;
                return slot;
            }
            return nullptr;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    bool Owns(const void* handle) const noexcept
    {
        std::lock_guard guard(mutex_);
        return IndexOf(handle) != kLimit;
    }

    bool Retire(const void* handle, unsigned int* id = nullptr) noexcept
    {
        std::lock_guard guard(mutex_);
        const size_t index = IndexOf(handle);
        if (index == kLimit) return false;
        if (id != nullptr) *id = slots_[index]->Id;
        slots_[index] = nullptr;
        return true;
    }

    size_t Live() const noexcept
    {
        std::lock_guard guard(mutex_);
        size_t live = 0;
        for (const Handle* slot : slots_) live += slot != nullptr;
        return live;
    }

    size_t Retained() const noexcept
    {
        std::lock_guard guard(mutex_);
        return issued_.size();
    }

private:
    size_t IndexOf(const void* handle) const noexcept
    {
        if (handle == nullptr) return kLimit;
        for (size_t index = 0; index < kLimit; ++index)
            if (slots_[index] == handle) return index;
        return kLimit;
    }

    mutable std::mutex mutex_;
    std::array<Handle*, kLimit> slots_{};
    std::vector<std::unique_ptr<Handle>> issued_;
    unsigned int nextId_ = kFirstId;
};
}
