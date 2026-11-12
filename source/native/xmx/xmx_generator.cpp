#include "xmx_generator.h"
#include "xmx_context.h"
#include "xmx_policy.h"
#include "xmx_status.h"
#include "tag_states.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace xmx::generator
{
namespace
{
using Microsoft::WRL::ComPtr;
using context::ExecuteParams;
using context::FrameData;
using context::InitParams;
using context::Input;

constexpr auto kBackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kInputState = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kXefgInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kGraphicsOnlyStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE
    | D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr uint64_t kStatusWindowMs = 10000;
constexpr uint32_t kSummaryLines = 60;
constexpr uint32_t kTimingSlots = 128;
constexpr uint32_t kTimingStamps = policy::kStampCount;
constexpr uint32_t kTimingStride = 64;
constexpr uint64_t kTimingMinAge = 6;
constexpr uint64_t kTimingLostAge = 48;

enum class Captured { eReady, eRefused, eFailed };
enum class Prepared { eReady, eFailed, eSettling };
enum class Verdict { eShown, eRepeat, eDeclined };

struct Copies
{
    ComPtr<ID3D12Resource> backbuffer, depth, motion, hudless;
    FrameData data{};
    bool valid{};
    policy::Idle refusal{policy::Idle::None};
};

struct PendingTiming
{
    uint64_t id{};
    uint32_t index{}, requested{};
    bool executed{};
};

struct Timing
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    uint8_t* mapped{};
    uint64_t frequency{};
    D3D12_COMMAND_LIST_TYPE listType{};
    uint64_t recorded{}, harvested{};
    std::array<PendingTiming, kTimingSlots> pending;
    policy::PairCost pair;
};

struct Costs
{
    window_stats::Ring<1024> execute, copies, total;
};

enum class Held { eInputCopy, eOther };

struct Retired
{
    ComPtr<IUnknown> object;
    Held kind{Held::eOther};
};

struct State
{
    Pair pair;
    ComPtr<ID3D12Device> device;
    void* context{};
    policy::ContextShape shape{};
    policy::ContextCache<void*> cache;
    policy::SettleClock settle;
    std::array<Copies, 2> frames;
    uint32_t current{};
    std::array<ComPtr<ID3D12Resource>, kMaxFrames> outputs;
    policy::RetireQueue<Retired> retired;
    Timing timing;
    uint64_t pairs{};
    uint64_t lastCaptureMs{};
    bool lastGenerated{};
    uint32_t contexts{};
    uint32_t maxFrames{1};
    uint32_t serves{1};
    D3D12_RESOURCE_DESC outputDesc{};
    bool failed{};
    bool refused{};
    policy::RefusedShapes refusals;
};

struct Summary
{
    uint64_t since{}, evaluates{}, generated{};
    std::array<uint64_t, static_cast<int>(policy::Idle::Count)> idle{};
    uint32_t lines{};
};

struct Outcome
{
    bool fatal{};
    bool generated{};
    policy::Idle idle{policy::Idle::None};
};

State& Get()
{
    static auto* state = new State;
    return *state;
}

std::mutex& Lock()
{
    static auto* lock = new std::mutex;
    return *lock;
}

Summary& Summarized()
{
    static auto* summary = new Summary;
    return *summary;
}

Costs& MeasuredCosts()
{
    static auto* costs = new Costs;
    return *costs;
}

ID3D12Resource* Resource(const NVSDK_NGX_Parameter* params, const char* name)
{
    ID3D12Resource* value = nullptr;
    return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : nullptr;
}

template<class T>
T Value(const NVSDK_NGX_Parameter* params, const char* name, T fallback)
{
    T value{};
    return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES initial,
    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Flags = policy::IsTypedDepth(desc.Format) ? flags | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : flags;
    desc.MipLevels = 1;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Alignment = 0;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
            IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

void CopyFirstSubresource(ID3D12GraphicsCommandList* list, ID3D12Resource* target, ID3D12Resource* source)
{
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = target;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = source;
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
}

bool Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState,
    ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState)
{
    const auto to = target->GetDesc(), from = source->GetDesc();
    if (!policy::CanCopy(to.Width, to.Height, to.Format, from.Width, from.Height, from.Format))
    {
        status::NoteIdle(policy::Idle::CopyMismatch);
        return false;
    }
    Barrier(list, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (policy::PickCopy(to.MipLevels, to.DepthOrArraySize, from.MipLevels, from.DepthOrArraySize)
        == policy::CopyMode::eWhole)
        list->CopyResource(target, source);
    else
        CopyFirstSubresource(list, target, source);
    Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    Barrier(list, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
    return true;
}

D3D12_RESOURCE_STATES TaggedState(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    D3D12_RESOURCE_STATES fallback)
{
    auto from = static_cast<D3D12_RESOURCE_STATES>(tag_states::StateOf(source, fallback));
    if (list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE && (from & kGraphicsOnlyStates) != 0)
        from = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return from;
}

bool SameShape(const ComPtr<ID3D12Resource>& copy, const D3D12_RESOURCE_DESC& desc)
{
    const auto current = copy->GetDesc();
    return current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format;
}

void Retire(State& state, ComPtr<IUnknown> object, Held kind)
{
    state.retired.Push({std::move(object), kind}, state.pairs);
}

Captured Capture(State& state, ID3D12GraphicsCommandList* list, ComPtr<ID3D12Resource>& copy, ID3D12Resource* source,
    D3D12_RESOURCE_STATES sourceState)
{
    if (source == nullptr)
    {
        if (copy) Retire(state, std::move(copy), Held::eInputCopy);
        copy.Reset();
        return Captured::eReady;
    }
    const auto desc = source->GetDesc();
    if (!copy || !SameShape(copy, desc))
    {
        if (copy) Retire(state, std::move(copy), Held::eInputCopy);
        copy = Texture(state.device.Get(), desc, kXefgInputState, D3D12_RESOURCE_FLAG_NONE);
        if (!copy) return policy::IsTypedDepth(desc.Format) ? Captured::eRefused : Captured::eFailed;
    }
    Copy(list, copy.Get(), kXefgInputState, source, sourceState);
    return Captured::eReady;
}

Input Describe(ID3D12Resource* copy)
{
    Input input{};
    input.resource = copy;
    if (copy != nullptr)
    {
        const auto desc = copy->GetDesc();
        input.width = static_cast<uint32_t>(desc.Width);
        input.height = desc.Height;
    }
    return input;
}

policy::Area DeclaredArea(const NVSDK_NGX_Parameter* params, const std::string& name, const Input& input)
{
    return policy::InputArea(input.width, input.height,
        {Value<unsigned int>(params, (name + "SubrectBaseX").c_str(), 0u),
            Value<unsigned int>(params, (name + "SubrectBaseY").c_str(), 0u),
            Value<unsigned int>(params, (name + "SubrectWidth").c_str(), 0u),
            Value<unsigned int>(params, (name + "SubrectHeight").c_str(), 0u)});
}

void Assign(Input& input, const policy::Area& area)
{
    input.baseX = area.x;
    input.baseY = area.y;
    input.width = area.width;
    input.height = area.height;
}

void MatchDepthAndMotion(const NVSDK_NGX_Parameter* params, FrameData& data)
{
    if (data.depth.width == data.motion.width && data.depth.height == data.motion.height) return;
    const auto [depth, motion] = policy::SharedArea(DeclaredArea(params, "DLSSG.Depth", data.depth),
        DeclaredArea(params, "DLSSG.MVecs", data.motion));
    static std::atomic_bool logged{false};
    if (!logged.exchange(true))
        Log("depth " + std::to_string(data.depth.width) + "x" + std::to_string(data.depth.height) + " and motion "
            + std::to_string(data.motion.width) + "x" + std::to_string(data.motion.height) + " differ, using "
            + std::to_string(depth.width) + "x" + std::to_string(depth.height) + " at depth "
            + std::to_string(depth.x) + "," + std::to_string(depth.y) + " motion " + std::to_string(motion.x) + ","
            + std::to_string(motion.y));
    Assign(data.depth, depth);
    Assign(data.motion, motion);
}

uint32_t ContextFlags(const NVSDK_NGX_Parameter* params)
{
    uint32_t flags = 0;
    if (Value<int>(params, "DLSSG.DepthInverted", 0) != 0) flags |= context::kFlagDepthInverted;
    if (Value<int>(params, "DLSSG.MvecJittered", 0) != 0) flags |= context::kFlagMotionJittered;
    return flags;
}

void RetireContext(State& state)
{
    const auto retire = [&](ComPtr<ID3D12Resource>& resource, Held kind)
    {
        if (resource) Retire(state, std::move(resource), kind);
        resource.Reset();
    };
    for (auto& frame : state.frames)
    {
        retire(frame.backbuffer, Held::eInputCopy);
        retire(frame.depth, Held::eInputCopy);
        retire(frame.motion, Held::eInputCopy);
        retire(frame.hudless, Held::eInputCopy);
        frame = {};
    }
    for (auto& texture : state.outputs) retire(texture, Held::eOther);
    state.context = nullptr;
    state.lastGenerated = false;
}

struct AdapterCache
{
    std::mutex lock;
    bool factoryTried{};
    ComPtr<IDXGIFactory4> factory;
    LUID luid{};
    ComPtr<IDXGIAdapter3> adapter;
};

AdapterCache& Adapters()
{
    static auto* cache = new AdapterCache;
    return *cache;
}

HMODULE SystemDxgi()
{
    std::wstring path(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (length == 0 || length >= path.size()) return nullptr;
    path.resize(length);
    path += L"\\dxgi.dll";
    return LoadLibraryW(path.c_str());
}

ComPtr<IDXGIFactory4> SystemFactory()
{
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = SystemDxgi();
    const auto create = dxgi != nullptr ? reinterpret_cast<Create>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    ComPtr<IDXGIFactory4> factory;
    if (create == nullptr || FAILED(create(IID_PPV_ARGS(&factory)))) return nullptr;
    return factory;
}

ComPtr<IDXGIAdapter3> AdapterOf(AdapterCache& cache, LUID luid)
{
    const bool sameAdapter = cache.luid.LowPart == luid.LowPart && cache.luid.HighPart == luid.HighPart;
    if (cache.adapter && sameAdapter) return cache.adapter;
    if (!cache.factoryTried)
    {
        cache.factoryTried = true;
        cache.factory = SystemFactory();
    }
    cache.adapter.Reset();
    if (!cache.factory) return nullptr;
    if (SUCCEEDED(cache.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&cache.adapter)))) cache.luid = luid;
    return cache.adapter;
}

bool LocalMemory(LUID luid, DXGI_QUERY_VIDEO_MEMORY_INFO& info)
{
    auto& cache = Adapters();
    std::lock_guard guard(cache.lock);
    const auto adapter = AdapterOf(cache, luid);
    return adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
}

bool NewContext(State& state)
{
    std::string failure;
    if (context::Create(state.pair.fg, *state.pair.rvas, state.device.Get(), state.context, failure)) return true;
    status::Fail(failure);
    return false;
}

int InitializeContext(State& state, InitParams& init)
{
    const auto result = context::Initialize(state.pair.fg, *state.pair.rvas, state.context, init);
    Log("init " + std::to_string(init.width) + "x" + std::to_string(init.height) + " format "
        + std::to_string(init.format) + " flags " + std::to_string(init.flags) + " frames "
        + std::to_string(init.maxFrames) + " result " + std::to_string(result));
    return result;
}

int InitializeWithFallback(State& state, InitParams& init, uint32_t flags, bool strip)
{
    auto result = InitializeContext(state, init);
    if (strip && result == 0) Log("the multi-frame model runs without the jittered-motion flag");
    init.flags = flags;
    if (result == 0 || init.maxFrames <= 1 || state.contexts >= policy::kMaxContexts) return result;
    ++state.contexts;
    if (!NewContext(state)) return result;
    init.maxFrames = 1;
    result = InitializeContext(state, init);
    if (result != 0) return result;
    Log("the multi-frame model refused these inputs; Intel's 2x model runs instead");
    return result;
}

std::string InitFailure(DXGI_FORMAT format, int result)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return "XMX cannot use this game's scRGB HDR output (result " + std::to_string(result)
            + "); turn HDR off or use XeMFG";
    return "XeFG initialization returned " + std::to_string(result);
}

void ClearRefusal(State& state)
{
    if (!state.refused) return;
    state.refused = false;
    status::ClearFailure();
}

void MarkRefused(State& state, const std::string& reason)
{
    state.refused = true;
    status::Fail(reason);
}

void Adopt(State& state, ID3D12Device* device, const D3D12_RESOURCE_DESC& output,
    const policy::ContextCache<void*>::Entry& entry)
{
    ClearRefusal(state);
    state.device = device;
    state.context = entry.handle;
    state.maxFrames = entry.frames;
    state.serves = entry.serves;
    state.outputDesc = output;
    state.shape = entry.shape;
}

void SwitchToCached(State& state, ID3D12Device* device, const D3D12_RESOURCE_DESC& output,
    const policy::ContextCache<void*>::Entry& cached)
{
    if (state.context != nullptr) RetireContext(state);
    Adopt(state, device, output, cached);
    Log("reusing the context made for " + std::to_string(cached.shape.width) + "x" + std::to_string(cached.shape.height));
}

InitParams InitFor(const policy::ContextShape& shape, uint32_t maxFrames, bool strip)
{
    InitParams init{};
    init.width = shape.width;
    init.height = shape.height;
    init.format = shape.format;
    init.flags = strip ? shape.flags & ~context::kFlagMotionJittered : shape.flags;
    init.maxFrames = maxFrames;
    init.creationMask = 1;
    init.visibleMask = 1;
    return init;
}

struct VideoMemory
{
    uint64_t used{}, budget{};
};

VideoMemory VideoMemoryOf(ID3D12Device* device)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!LocalMemory(device->GetAdapterLuid(), info)) return {};
    return {info.CurrentUsage, info.Budget};
}

std::string ContextCostText(const policy::ContextShape& shape, uint32_t frames, const VideoMemory& before,
    const VideoMemory& after)
{
    const auto usedBefore = static_cast<long long>(before.used >> 20);
    const auto usedAfter = static_cast<long long>(after.used >> 20);
    char text[200]{};
    std::snprintf(text, sizeof(text), "context %ux%u frames %u: used %lld -> %lld MB (%+lld), budget %lld MB",
        shape.width, shape.height, frames, usedBefore, usedAfter, usedAfter - usedBefore,
        static_cast<long long>(after.budget >> 20));
    return text;
}

Prepared CreateForShape(State& state, ID3D12Device* device, const policy::ContextShape& wanted, uint32_t frames,
    const D3D12_RESOURCE_DESC& output)
{
    if (state.contexts >= policy::kMaxContexts)
    {
        status::Fail("the game changed its output size too many times; restart the game");
        state.failed = true;
        return Prepared::eFailed;
    }
    if (state.context != nullptr) RetireContext(state);
    state.device = device;
    const auto before = VideoMemoryOf(device);
    ++state.contexts;
    if (!NewContext(state))
    {
        state.failed = true;
        return Prepared::eFailed;
    }
    const bool strip = (wanted.flags & context::kFlagMotionJittered) != 0 && frames > 1;
    auto init = InitFor(wanted, frames, strip);
    const auto result = InitializeWithFallback(state, init, wanted.flags, strip);
    if (result != 0)
    {
        const auto reason = InitFailure(output.Format, result);
        MarkRefused(state, reason);
        state.context = nullptr;
        state.refusals.Add(wanted, reason);
        return Prepared::eFailed;
    }
    const bool fellBack = init.maxFrames < frames;
    const policy::ContextCache<void*>::Entry entry{wanted, state.context, init.maxFrames,
        fellBack ? state.pair.maxFrames : init.maxFrames};
    state.cache.Add(entry);
    Adopt(state, device, output, entry);
    Log(ContextCostText(wanted, entry.frames, before, VideoMemoryOf(device)));
    return Prepared::eReady;
}

Prepared Prepare(State& state, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params,
    uint32_t requested, const D3D12_RESOURCE_DESC& output)
{
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return Prepared::eFailed;
    const policy::ContextShape wanted{device.Get(), static_cast<uint32_t>(output.Width), output.Height,
        static_cast<uint32_t>(output.Format), ContextFlags(params)};
    const auto frames = policy::FramesFor(requested, state.pair.maxFrames);
    std::optional<policy::CurrentContext> current;
    if (state.context != nullptr) current = policy::CurrentContext{state.shape, state.serves};
    const auto choice = policy::ChooseContext(state.cache, current, wanted, frames);
    if (choice.action == policy::ContextAction::eKeep)
    {
        state.settle.Clear();
        ClearRefusal(state);
        return Prepared::eReady;
    }
    if (choice.action == policy::ContextAction::eSwitch)
    {
        state.settle.Clear();
        SwitchToCached(state, device.Get(), output, *choice.cached);
        return Prepared::eReady;
    }
    if (const auto* refusal = state.refusals.Find(wanted))
    {
        if (!state.refused) MarkRefused(state, refusal->reason);
        return Prepared::eFailed;
    }
    if (state.context != nullptr && !state.settle.Settled(wanted, GetTickCount64())) return Prepared::eSettling;
    state.settle.Clear();
    return CreateForShape(state, device.Get(), wanted, frames, output);
}

std::string Shape(const NVSDK_NGX_Parameter* params, const char* name)
{
    auto* resource = Resource(params, name);
    if (resource == nullptr) return "none";
    const auto desc = resource->GetDesc();
    return std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + "/"
        + std::to_string(static_cast<int>(desc.Format)) + " mips " + std::to_string(desc.MipLevels) + " array "
        + std::to_string(desc.DepthOrArraySize);
}

std::string CostText()
{
    auto& costs = MeasuredCosts();
    const auto execute = costs.execute.Drain();
    const auto copies = costs.copies.Drain();
    const auto total = costs.total.Drain();
    return policy::GpuCostText(execute, copies, total);
}

std::string SummaryText(const Summary& summary, const NVSDK_NGX_Parameter* params)
{
    std::string text = "summary 10 s: evaluates " + std::to_string(summary.evaluates) + ", generated "
        + std::to_string(summary.generated);
    for (int i = 1; i < static_cast<int>(policy::Idle::Count); ++i)
        if (summary.idle[i] != 0)
            text += ", " + std::string(policy::IdleText(static_cast<policy::Idle>(i))) + " "
                + std::to_string(summary.idle[i]);
    return text + "; depth " + Shape(params, "DLSSG.Depth") + " motion " + Shape(params, "DLSSG.MVecs")
        + " hudless " + Shape(params, "DLSSG.HUDLess") + " backbuffer " + Shape(params, "DLSSG.Backbuffer")
        + "; mv scale " + std::to_string(Value<float>(params, "DLSSG.MvecScaleX", 0.f)) + ","
        + std::to_string(Value<float>(params, "DLSSG.MvecScaleY", 0.f)) + " reset "
        + std::to_string(Value<unsigned int>(params, "DLSSG.Reset", 0)) + " frames "
        + std::to_string(Value<unsigned int>(params, "DLSSG.MultiFrameCount", 0)) + "; " + CostText();
}

void Summarize(const NVSDK_NGX_Parameter* params, bool generated, policy::Idle idle)
{
    auto& summary = Summarized();
    const auto now = GetTickCount64();
    if (summary.since == 0) summary.since = now;
    ++summary.evaluates;
    if (generated) ++summary.generated;
    ++summary.idle[static_cast<int>(idle)];
    if (now - summary.since < kStatusWindowMs) return;
    if (summary.lines++ < kSummaryLines) LogSummary(SummaryText(summary, params));
    const auto lines = summary.lines;
    summary = {};
    summary.lines = lines;
    summary.since = now;
}

uint32_t Microseconds(float milliseconds)
{
    return static_cast<uint32_t>(std::lround(std::max(milliseconds, 0.f) * 1000.f));
}

void AddCost(Timing& timing, const PendingTiming& entry, const policy::StampCost& cost)
{
    const auto frame = timing.pair.Add(entry.index, entry.requested, cost.executeMs, cost.copyMs, entry.executed);
    if (!frame || frame->executes == 0) return;
    auto& costs = MeasuredCosts();
    costs.execute.Add(Microseconds(frame->executeMs));
    costs.copies.Add(Microseconds(frame->copyMs));
    costs.total.Add(Microseconds(frame->executeMs + frame->copyMs));
}

bool Harvest(Timing& timing, uint64_t id)
{
    const auto slot = static_cast<size_t>((id - 1) % kTimingSlots);
    const auto& entry = timing.pending[slot];
    if (entry.id != id) return true;
    std::array<uint64_t, kTimingStamps> stamps{};
    std::memcpy(stamps.data(), timing.mapped + slot * kTimingStride, sizeof(stamps));
    const auto cost = policy::CostOfStamps(stamps, timing.frequency);
    if (!cost) return timing.recorded - id >= kTimingLostAge;
    AddCost(timing, entry, *cost);
    return true;
}

void Reap(Timing& timing)
{
    while (timing.recorded - timing.harvested > kTimingMinAge)
    {
        const uint64_t id = timing.harvested + 1;
        if (!Harvest(timing, id)) return;
        timing.harvested = id;
    }
}

UINT64 TimestampFrequency(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = type;
    ComPtr<ID3D12CommandQueue> queue;
    UINT64 frequency = 0;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))
        || FAILED(queue->GetTimestampFrequency(&frequency))) return 0;
    return frequency;
}

bool CreateTimestampBuffers(ID3D12Device* device, Timing& timing)
{
    const D3D12_QUERY_HEAP_DESC heapDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimingSlots * kTimingStamps, 0};
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = uint64_t{kTimingSlots} * kTimingStride;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(buffer.Width)};
    void* mapped = nullptr;
    const bool created = SUCCEEDED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&timing.heap)))
        && SUCCEEDED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timing.readback)))
        && SUCCEEDED(timing.readback->Map(0, &readRange, &mapped)) && mapped != nullptr;
    if (!created)
    {
        timing.heap.Reset();
        timing.readback.Reset();
        return false;
    }
    timing.mapped = static_cast<uint8_t*>(mapped);
    return true;
}

bool CreateTiming(State& state, D3D12_COMMAND_LIST_TYPE type)
{
    auto& timing = state.timing;
    timing = {};
    timing.device = state.device;
    timing.listType = type;
    const auto unavailable = [](const char* why)
    {
        Log(std::string("gpu timing unavailable: ") + why);
        return false;
    };
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return unavailable("the command list is neither direct nor compute");
    timing.frequency = TimestampFrequency(state.device.Get(), type);
    if (timing.frequency == 0) return unavailable("the timestamp frequency could not be read");
    if (!CreateTimestampBuffers(state.device.Get(), timing))
        return unavailable("the query heap or its readback buffer could not be created");
    return true;
}

bool TimingReady(State& state, D3D12_COMMAND_LIST_TYPE type)
{
    auto& timing = state.timing;
    if (timing.device.Get() == state.device.Get() && timing.listType == type) return timing.heap != nullptr;
    if (timing.heap) Retire(state, std::move(timing.heap), Held::eOther);
    if (timing.readback) Retire(state, std::move(timing.readback), Held::eOther);
    return CreateTiming(state, type);
}

class GpuTimer
{
public:
    GpuTimer(State& state, ID3D12GraphicsCommandList* list) : timing(state.timing), list(list)
    {
        if (!TimingReady(state, list->GetType())) return;
        Reap(timing);
        id = ++timing.recorded;
        slot = static_cast<uint32_t>((id - 1) % kTimingSlots);
        timing.pending[slot] = {};
        std::memset(timing.mapped + uint64_t{slot} * kTimingStride, 0, kTimingStride);
        active = true;
        Mark(policy::kStampStart);
    }

    void Mark(uint32_t stamp)
    {
        if (active) list->EndQuery(timing.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kTimingStamps + stamp);
    }

    void Finish(uint32_t index, uint32_t requested, bool executed)
    {
        if (!active) return;
        Mark(policy::kStampShown);
        list->ResolveQueryData(timing.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kTimingStamps, kTimingStamps,
            timing.readback.Get(), uint64_t{slot} * kTimingStride);
        timing.pending[slot] = {id, index, requested, executed};
    }

private:
    Timing& timing;
    ID3D12GraphicsCommandList* list;
    uint64_t id{};
    uint32_t slot{};
    bool active{};
};

Captured CaptureInputs(State& state, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params,
    Copies& next, ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES backbufferState, ID3D12Resource* depth,
    ID3D12Resource* motion)
{
    struct InputCopy
    {
        ComPtr<ID3D12Resource>& copy;
        ID3D12Resource* source;
        D3D12_RESOURCE_STATES state;
    };
    auto* const hudless = Resource(params, "DLSSG.HUDLess");
    const InputCopy inputs[] = {
        {next.backbuffer, backbuffer, backbufferState},
        {next.depth, depth, TaggedState(list, depth, kInputState)},
        {next.motion, motion, TaggedState(list, motion, kInputState)},
        {next.hudless, hudless, TaggedState(list, hudless, kInputState)},
    };
    for (const auto& input : inputs)
    {
        const auto captured = Capture(state, list, input.copy, input.source, input.state);
        if (captured != Captured::eReady) return captured;
    }
    return Captured::eReady;
}

void DescribeFrame(const State& state, const NVSDK_NGX_Parameter* params, Copies& next)
{
    auto& data = next.data;
    data = {};
    data.backbuffer = Describe(next.backbuffer.Get());
    data.depth = Describe(next.depth.Get());
    data.motion = Describe(next.motion.Get());
    MatchDepthAndMotion(params, data);
    data.hudless = Describe(next.hudless.Get());
    void* projection = nullptr;
    if (params->Get("DLSSG.CameraViewToClip", &projection) == NVSDK_NGX_Result_Success && projection != nullptr)
        std::memcpy(data.projection, projection, sizeof(data.projection));
    data.resolution[0] = static_cast<float>(state.shape.width);
    data.resolution[1] = static_cast<float>(state.shape.height);
    data.jitter[0] = Value<float>(params, "DLSSG.JitterOffsetX", 0.f);
    data.jitter[1] = Value<float>(params, "DLSSG.JitterOffsetY", 0.f);
    data.motionScale[0] = Value<float>(params, "DLSSG.MvecScaleX", 1.f);
    data.motionScale[1] = Value<float>(params, "DLSSG.MvecScaleY", 1.f);
    data.reset = Value<unsigned int>(params, "DLSSG.Reset", 0) != 0 ? 1 : 0;
}

Captured CapturePair(State& state, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params,
    ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES backbufferState, ID3D12Resource* depth, ID3D12Resource* motion)
{
    state.retired.Collect(++state.pairs);
    const auto now = GetTickCount64();
    if (policy::PairIsStale(state.lastCaptureMs, now)) state.frames[state.current].valid = false;
    state.lastCaptureMs = now;
    state.current ^= 1;
    auto& next = state.frames[state.current];
    next.valid = false;
    next.refusal = policy::Idle::None;
    const auto captured = CaptureInputs(state, list, params, next, backbuffer, backbufferState, depth, motion);
    if (captured == Captured::eRefused)
    {
        next.refusal = policy::Idle::DepthFormat;
        return captured;
    }
    if (captured == Captured::eFailed)
    {
        status::Fail("input copies");
        state.failed = true;
        return captured;
    }
    DescribeFrame(state, params, next);
    next.valid = true;
    return Captured::eReady;
}

bool OutputsReady(State& state, uint32_t count, ID3D12Resource* (&outputs)[kMaxFrames])
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!state.outputs[i])
            state.outputs[i] = Texture(state.device.Get(), state.outputDesc, kOutputState,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!state.outputs[i]) return false;
        outputs[i] = state.outputs[i].Get();
    }
    return true;
}

Outcome ExecuteModel(State& state, ID3D12GraphicsCommandList* list, const policy::FramePlan& plan)
{
    auto& next = state.frames[state.current];
    auto& previous = state.frames[state.current ^ 1];
    ExecuteParams execute{};
    execute.previous = previous.data;
    execute.next = next.data;
    ID3D12Resource* outputs[kMaxFrames]{};
    if (!OutputsReady(state, plan.count, outputs))
    {
        status::Fail("output texture");
        state.failed = true;
        return {true, false, policy::Idle::None};
    }
    const uint32_t bases[kMaxFrames * 2]{};
    execute.outputs = outputs;
    execute.outputBases = bases;
    execute.outputCount = static_cast<uint8_t>(plan.count);
    execute.firstFrame = plan.index - 1;
    execute.frameCount = 1;
    const auto result = context::Execute(state.pair.fg, *state.pair.rvas, state.context, list, execute);
    if (result < 0)
    {
        Log("execute failed: " + std::to_string(result));
        if (result == context::kCrashed)
        {
            status::Fail("XeFG crashed while generating a frame");
            state.failed = true;
        }
        return {false, false, policy::Idle::ExecuteError};
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) Log("execute result " + std::to_string(result));
    return {false, result == 0, result == 0 ? policy::Idle::None : policy::Idle::NoOutput};
}

policy::Idle PairIdle(const State& state)
{
    const auto& next = state.frames[state.current];
    const auto& previous = state.frames[state.current ^ 1];
    if (next.refusal != policy::Idle::None) return next.refusal;
    if (!previous.valid) return policy::Idle::NoPair;
    if (next.data.reset != 0) return policy::Idle::GameReset;
    if ((next.hudless != nullptr) != (previous.hudless != nullptr)) return policy::Idle::HudlessChanged;
    return policy::Idle::None;
}

void NoteFramesLimit(const State& state, uint32_t requested)
{
    if (status::framesLimit.exchange(state.maxFrames) != 0) return;
    Log(std::to_string(requested) + " generated frames asked for, this model produces "
        + std::to_string(state.maxFrames));
}

bool ShowFrame(State& state, ID3D12GraphicsCommandList* list, ID3D12Resource* interpolated,
    ID3D12Resource* backbuffer, D3D12_RESOURCE_STATES backbufferState, bool generated, uint32_t index)
{
    if (generated) return Copy(list, interpolated, kOutputState, state.outputs[index - 1].Get(), kOutputState);
    return Copy(list, interpolated, kOutputState, backbuffer, backbufferState);
}

void RecordOutcome(State& state, const NVSDK_NGX_Parameter* params, const Outcome& outcome)
{
    state.lastGenerated = outcome.generated;
    if (outcome.generated)
    {
        status::lastGeneratedTick.store(GetTickCount64(), std::memory_order_relaxed);
        status::generated.fetch_add(1, std::memory_order_relaxed);
    }
    status::NoteIdle(outcome.idle);
    Summarize(params, outcome.generated, outcome.idle);
}

struct HeldMemory
{
    uint32_t contexts{};
    uint64_t copies{};
    uint64_t outputs{};
};

uint64_t AllocatedBytes(ID3D12Device* device, ID3D12Resource* resource)
{
    if (resource == nullptr) return 0;
    const auto desc = resource->GetDesc();
    return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

uint64_t RetiredCopyBytes(const State& state)
{
    uint64_t bytes = 0;
    state.retired.ForEach([&](const Retired& retired)
    {
        ComPtr<ID3D12Resource> resource;
        if (retired.kind == Held::eInputCopy && SUCCEEDED(retired.object.As(&resource)))
            bytes += AllocatedBytes(state.device.Get(), resource.Get());
    });
    return bytes;
}

HeldMemory MeasureHeld(const State& state)
{
    HeldMemory held{state.contexts, RetiredCopyBytes(state), 0};
    for (const auto& frame : state.frames)
        for (auto* copy : {frame.backbuffer.Get(), frame.depth.Get(), frame.motion.Get(), frame.hudless.Get()})
            held.copies += AllocatedBytes(state.device.Get(), copy);
    for (const auto& output : state.outputs) held.outputs += AllocatedBytes(state.device.Get(), output.Get());
    return held;
}

std::string MemoryText(const DXGI_QUERY_VIDEO_MEMORY_INFO& info, const HeldMemory& held)
{
    char text[256]{};
    std::snprintf(text, sizeof(text),
        "VRAM 10 s: used %llu MB of budget %llu MB, contexts %u, copies %llu MB, outputs %llu MB",
        static_cast<unsigned long long>(info.CurrentUsage >> 20), static_cast<unsigned long long>(info.Budget >> 20),
        held.contexts, static_cast<unsigned long long>(held.copies >> 20),
        static_cast<unsigned long long>(held.outputs >> 20));
    return text;
}

Verdict Decline(State& state, const NVSDK_NGX_Parameter* params)
{
    state.frames[state.current].valid = false;
    state.lastGenerated = false;
    status::NoteIdle(policy::Idle::OutputSettling);
    Summarize(params, false, policy::Idle::OutputSettling);
    return Verdict::eDeclined;
}

D3D12_RESOURCE_STATES BackbufferState(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params)
{
    return TaggedState(list, Resource(params, "DLSSG.Backbuffer"), kBackbufferState);
}

Verdict Generate(State& state, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params,
    D3D12_RESOURCE_STATES backbufferState)
{
    if (state.failed) return Verdict::eRepeat;
    auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
    auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
    auto* real = Resource(params, "DLSSG.OutputReal");
    auto* depth = Resource(params, "DLSSG.Depth");
    auto* motion = Resource(params, "DLSSG.MVecs");
    if (backbuffer == nullptr || interpolated == nullptr || depth == nullptr || motion == nullptr)
    {
        status::NoteIdle(policy::Idle::MissingInputs);
        Summarize(params, false, policy::Idle::MissingInputs);
        return Verdict::eRepeat;
    }
    const auto requested = std::max(Value<unsigned int>(params, "DLSSG.MultiFrameCount", 1), 1u);
    const auto requestedIndex = std::clamp(Value<unsigned int>(params, "DLSSG.MultiFrameIndex", 1), 1u, requested);
    const auto prepared = Prepare(state, list, params, requested, interpolated->GetDesc());
    if (prepared == Prepared::eSettling) return Decline(state, params);
    if (prepared == Prepared::eFailed) return Verdict::eRepeat;
    const auto plan = policy::PlanFrames(requested, requestedIndex, state.maxFrames);
    if (plan.limited) NoteFramesLimit(state, requested);
    GpuTimer timer(state, list);
    if (real != nullptr && !Copy(list, real, kOutputState, backbuffer, backbufferState)) return Verdict::eDeclined;
    timer.Mark(policy::kStampRealCopied);
    if (plan.repeat)
    {
        timer.Mark(policy::kStampCaptured);
        timer.Mark(policy::kStampExecuted);
        if (!ShowFrame(state, list, interpolated, backbuffer, backbufferState, state.lastGenerated, plan.index))
            return Verdict::eDeclined;
        timer.Finish(requestedIndex, requested, false);
        return Verdict::eShown;
    }
    if (requestedIndex == 1
        && CapturePair(state, list, params, backbuffer, backbufferState, depth, motion) == Captured::eFailed)
        return Verdict::eRepeat;
    timer.Mark(policy::kStampCaptured);
    Outcome outcome{false, false, PairIdle(state)};
    const bool executes = outcome.idle == policy::Idle::None;
    if (executes)
    {
        outcome = ExecuteModel(state, list, plan);
        if (outcome.fatal) return Verdict::eRepeat;
    }
    timer.Mark(policy::kStampExecuted);
    RecordOutcome(state, params, outcome);
    if (!ShowFrame(state, list, interpolated, backbuffer, backbufferState, outcome.generated, plan.index))
        return Verdict::eDeclined;
    timer.Finish(requestedIndex, requested, executes);
    return Verdict::eShown;
}

bool RepeatRealFrame(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params,
    D3D12_RESOURCE_STATES backbufferState)
{
    auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
    auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
    if (backbuffer == nullptr || interpolated == nullptr) return false;
    auto* const real = Resource(params, "DLSSG.OutputReal");
    if (real != nullptr && !Copy(list, real, kOutputState, backbuffer, backbufferState)) return false;
    return Copy(list, interpolated, kOutputState, backbuffer, backbufferState);
}
}

bool Passthrough(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) noexcept
{
    if (list == nullptr || params == nullptr) return false;
    try
    {
        return RepeatRealFrame(list, params, BackbufferState(list, params));
    }
    catch (...)
    {
        return false;
    }
}

void Attach(const Pair& pair) noexcept
{
    std::lock_guard guard(Lock());
    auto& state = Get();
    state.pair = pair;
}

std::string MemoryLine() noexcept
{
    try
    {
        LUID luid{};
        HeldMemory held;
        {
            std::lock_guard guard(Lock());
            const auto& state = Get();
            if (!state.device) return {};
            luid = state.device->GetAdapterLuid();
            held = MeasureHeld(state);
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!LocalMemory(luid, info)) return {};
        return MemoryText(info, held);
    }
    catch (...)
    {
        return {};
    }
}

std::string UsedLine(ID3D12GraphicsCommandList* list) noexcept
{
    try
    {
        ComPtr<ID3D12Device> device;
        if (list == nullptr || FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return {};
        const auto memory = VideoMemoryOf(device.Get());
        if (memory.budget == 0) return {};
        return "used " + std::to_string(memory.used >> 20) + " MB of budget " + std::to_string(memory.budget >> 20)
            + " MB";
    }
    catch (...)
    {
        return {};
    }
}

static void NoteUnexpectedFailure() noexcept
{
    try
    {
        status::Fail("unexpected error while generating a frame");
    }
    catch (...) {}
}

bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) noexcept
{
    if (list == nullptr || params == nullptr) return false;
    try
    {
        std::lock_guard guard(Lock());
        auto& state = Get();
        if (state.pair.fg == nullptr || state.pair.rvas == nullptr) return false;
        status::evaluates.fetch_add(1, std::memory_order_relaxed);
        const auto backbufferState = BackbufferState(list, params);
        const auto verdict = Generate(state, list, params, backbufferState);
        if (verdict == Verdict::eDeclined) return false;
        return verdict == Verdict::eShown || RepeatRealFrame(list, params, backbufferState);
    }
    catch (...)
    {
        NoteUnexpectedFailure();
        return false;
    }
}
}
