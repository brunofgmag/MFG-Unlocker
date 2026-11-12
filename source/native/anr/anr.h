#pragma once
#include <nvsdk_ngx_defs.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include "anr_rules.h"
#include "xmx/xmx.h"

namespace anr
{
struct Status
{
    bool latched = false;
    bool loaded = false;
    bool evaluating = false;
    size_t live = 0;
    uint64_t creates = 0;
    uint64_t evaluates = 0;
    uint64_t failedEvaluates = 0;
    std::string refusal;
    std::string runtime;
    std::string failure;
};

Selection Latch(bool requested, bool xmxRequested, bool standaloneRequested) noexcept;
bool Latched() noexcept;
bool AnswerNgxCall(xmx::ProviderCall call, const xmx::ProviderArgs& args, NVSDK_NGX_Result& result) noexcept;
Status ReadStatus() noexcept;
}
