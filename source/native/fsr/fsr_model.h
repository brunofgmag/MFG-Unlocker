#pragma once
#include "xmx/xmx_model.h"

namespace fsr
{
struct Stats
{
    uint64_t pairs, dispatches, resets, refusals;
};

const xmx::generator::Model& Model() noexcept;
Stats ReadStats() noexcept;
}
