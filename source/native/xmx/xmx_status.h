#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include "xmx_policy.h"

namespace xmx
{
void Log(const std::string& text) noexcept;
void LogSummary(const std::string& text) noexcept;
}

namespace xmx::status
{
inline std::atomic<uint64_t> lastGeneratedTick{0}, lastEvaluateTick{0}, evaluates{0}, generated{0};
inline std::atomic<int> idle{0};
inline std::atomic<uint32_t> idleLogged{0}, framesLimit{0};
inline std::atomic<bool> hooked{false}, hookFailed{false};
inline std::mutex failureLock;
inline std::string failure;

inline void Fail(const std::string& reason) noexcept
{
    try
    {
        {
            std::lock_guard guard(failureLock);
            failure = reason;
        }
        Log("failed: " + reason);
    }
    catch (...) {}
}

inline void ClearFailure() noexcept
{
    try
    {
        std::lock_guard guard(failureLock);
        failure.clear();
    }
    catch (...) {}
}

inline std::string Failure()
{
    std::lock_guard guard(failureLock);
    return failure;
}

inline void NoteIdle(policy::Idle reason) noexcept
{
    idle.store(static_cast<int>(reason), std::memory_order_relaxed);
    if (reason == policy::Idle::None) return;
    const uint32_t bit = 1u << static_cast<int>(reason);
    if ((idleLogged.fetch_or(bit, std::memory_order_relaxed) & bit) != 0) return;
    try
    {
        Log(std::string("not generating: ") + policy::IdleText(reason));
    }
    catch (...) {}
}
}
