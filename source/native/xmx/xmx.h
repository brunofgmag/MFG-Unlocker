#pragma once
#include <nvsdk_ngx_defs.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include "xmx_rules.h"

namespace sl { struct ResourceTag; }

namespace xmx
{
struct Status
{
    bool latched = false;
    bool hooked = false;
    bool partiallyHooked = false;
    bool generating = false;
    bool evaluating = false;
    bool bypassed = false;
    uint32_t framesLimit = 0;
    uint64_t evaluates = 0;
    uint64_t generated = 0;
    std::string refusal;
    std::string idle;
    std::string failure;
};

enum class NgxLayer { eRuntime, eProvider };
enum class ProviderCall { eCreate, eEvaluate, eRelease };

struct ProviderArgs
{
    void* commandList;
    uintptr_t featureOrHandle;
    const void* parameters;
    void* outHandle;
};

Selection Latch(bool requested, bool standaloneRequested) noexcept;
bool Latched() noexcept;
void NoteProviderRoute(bool installed) noexcept;
void BeforeCreate() noexcept;
void NoteCreateMemory(void* commandList) noexcept;
bool Evaluate(void* commandList, const void* parameters, NVSDK_NGX_Result& result) noexcept;
bool AnswerNgxCall(NgxLayer layer, ProviderCall call, const ProviderArgs& args, NVSDK_NGX_Result& result) noexcept;
bool ConfigureStandIn(bool on) noexcept;
size_t StandInLive() noexcept;
void RecordTagStates(const sl::ResourceTag* tags, uint32_t count) noexcept;
Status ReadStatus(bool driverGenerating) noexcept;
std::string MemoryLine() noexcept;
}
