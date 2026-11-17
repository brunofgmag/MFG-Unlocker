#include <Windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "xmx/xmx_context.h"
#include "xmx/xmx_status.h"
#include "xmx/xmx_tensor_cores.h"
#include "xess/cuda/provider_list.h"
#include "xess/xess_mod.h"

namespace
{
unsigned gChecks = 0, gFailures = 0;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

struct Event
{
    std::string what;
    uint64_t token;
    const void* list;
};

std::vector<Event> gEvents;
std::vector<std::string> gLogs, gReports;
thread_local uint64_t gToken = 0;
xefg_cuda::State gState = xefg_cuda::State::NotAttempted;
xefg_cuda::Status gStatus;
bool gCanRecord = true;
int gExecuteResult = 0;
unsigned gInstallCalls = 0;

HRESULT STDMETHODCALLTYPE FakeQuery(void*, REFIID, void**)
{
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FakeRef(void*)
{
    return 1;
}

void* gTable[3] = {reinterpret_cast<void*>(&FakeQuery), reinterpret_cast<void*>(&FakeRef),
    reinterpret_cast<void*>(&FakeRef)};
struct FakeObject
{
    void** table;
    GUID iid;
    FakeObject* inner;
    ULONG refs;
    unsigned dispatches, heapSets, heapCount;
    ID3D12DescriptorHeap* heaps[2];
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    SIZE_T cpuStart;
} gFakeList{gTable};

ID3D12GraphicsCommandList* RawList()
{
    return reinterpret_cast<ID3D12GraphicsCommandList*>(&gFakeList);
}

const GUID kReshadeUnwrapped = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
constexpr size_t kListSlots = 70, kHeapSlots = 11;
constexpr SIZE_T kNativeViewStart = 0x1000, kWrappedViewStart = 0x2000, kPlainViewStart = 0x5000, kOpaqueViewStart = 0x6000;
constexpr size_t kDispatchSlot = 14, kPipelineStateSlot = 25, kSetHeapsSlot = 28, kGetDescSlot = 8, kGetCpuStartSlot = 9;

FakeObject gNativeList, gWrappedList, gNativeView, gWrappedView, gNativeSampler, gWrappedSampler, gPlainView, gOpaqueView;
void* gListTable[kListSlots];
void* gHeapTable[kHeapSlots];
void* gOpaqueTable[kHeapSlots];

FakeObject* Self(void* object)
{
    return static_cast<FakeObject*>(object);
}

HRESULT STDMETHODCALLTYPE ObjectQuery(void* self, REFIID riid, void** out)
{
    auto* object = Self(self);
    FakeObject* answer = nullptr;
    if (riid == __uuidof(IUnknown) || riid == object->iid) answer = object;
    else if (riid == kReshadeUnwrapped) answer = object->inner;
    *out = answer;
    if (!answer) return E_NOINTERFACE;
    ++answer->refs;
    return S_OK;
}

ULONG STDMETHODCALLTYPE ObjectAddRef(void* self)
{
    return ++Self(self)->refs;
}

ULONG STDMETHODCALLTYPE ObjectRelease(void* self)
{
    return --Self(self)->refs;
}

void STDMETHODCALLTYPE Unused()
{
}

void STDMETHODCALLTYPE CountDispatch(void* self, UINT, UINT, UINT)
{
    ++Self(self)->dispatches;
}

void STDMETHODCALLTYPE CountSetHeaps(void* self, UINT count, ID3D12DescriptorHeap* const* heaps)
{
    auto* object = Self(self);
    ++object->heapSets;
    object->heapCount = count;
    for (UINT i = 0; i < count && i < 2; ++i) object->heaps[i] = heaps[i];
}

D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE ObjectDesc(void* self, D3D12_DESCRIPTOR_HEAP_DESC* out)
{
    *out = {};
    out->Type = Self(self)->type;
    return out;
}

D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE ObjectCpuStart(void* self, D3D12_CPU_DESCRIPTOR_HANDLE* out)
{
    out->ptr = Self(self)->cpuStart;
    return out;
}

void Make(FakeObject& object, void** table, const GUID& iid, FakeObject* inner, D3D12_DESCRIPTOR_HEAP_TYPE type, SIZE_T cpuStart = 0)
{
    object = {};
    object.table = table;
    object.iid = iid;
    object.inner = inner;
    object.refs = 1;
    object.type = type;
    object.cpuStart = cpuStart;
}

void BuildWorld()
{
    for (auto& slot : gListTable) slot = reinterpret_cast<void*>(&Unused);
    for (auto& slot : gHeapTable) slot = reinterpret_cast<void*>(&Unused);
    gListTable[0] = gHeapTable[0] = reinterpret_cast<void*>(&ObjectQuery);
    gListTable[1] = gHeapTable[1] = reinterpret_cast<void*>(&ObjectAddRef);
    gListTable[2] = gHeapTable[2] = reinterpret_cast<void*>(&ObjectRelease);
    gListTable[kDispatchSlot] = reinterpret_cast<void*>(&CountDispatch);
    gListTable[kSetHeapsSlot] = reinterpret_cast<void*>(&CountSetHeaps);
    gHeapTable[kGetDescSlot] = reinterpret_cast<void*>(&ObjectDesc);
    gHeapTable[kGetCpuStartSlot] = reinterpret_cast<void*>(&ObjectCpuStart);
    for (auto& slot : gOpaqueTable) slot = reinterpret_cast<void*>(&Unused);
    gOpaqueTable[0] = reinterpret_cast<void*>(&FakeQuery);
    gOpaqueTable[1] = gOpaqueTable[2] = reinterpret_cast<void*>(&FakeRef);
    gOpaqueTable[kGetDescSlot] = reinterpret_cast<void*>(&ObjectDesc);
    gOpaqueTable[kGetCpuStartSlot] = reinterpret_cast<void*>(&ObjectCpuStart);
    const auto view = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, sampler = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    Make(gNativeList, gListTable, __uuidof(ID3D12GraphicsCommandList), nullptr, view);
    Make(gWrappedList, gListTable, __uuidof(ID3D12GraphicsCommandList), &gNativeList, view);
    Make(gNativeView, gHeapTable, __uuidof(ID3D12DescriptorHeap), nullptr, view, kNativeViewStart);
    Make(gWrappedView, gHeapTable, __uuidof(ID3D12DescriptorHeap), &gNativeView, view, kWrappedViewStart);
    Make(gNativeSampler, gHeapTable, __uuidof(ID3D12DescriptorHeap), nullptr, sampler, 0x3000);
    Make(gWrappedSampler, gHeapTable, __uuidof(ID3D12DescriptorHeap), &gNativeSampler, sampler, 0x4000);
    Make(gPlainView, gHeapTable, __uuidof(ID3D12DescriptorHeap), nullptr, view, kPlainViewStart);
    Make(gOpaqueView, gOpaqueTable, __uuidof(ID3D12DescriptorHeap), nullptr, view, kOpaqueViewStart);
}

ID3D12GraphicsCommandList* AsList(FakeObject& object)
{
    return reinterpret_cast<ID3D12GraphicsCommandList*>(&object);
}

ID3D12DescriptorHeap* AsHeap(FakeObject& object)
{
    return reinterpret_cast<ID3D12DescriptorHeap*>(&object);
}

ID3D12GraphicsCommandList* NativeList()
{
    BuildWorld();
    return AsList(gNativeList);
}

ID3D12GraphicsCommandList* WrappedList()
{
    BuildWorld();
    return AsList(gWrappedList);
}

uint8_t gLayer = 0;
bool gLaunchResult = false;
bool gUnderWine = true;
uint64_t gNextToken = 40;
SIZE_T gLaunchNativeStart = 0;
unsigned gLaunchCalls = 0;
ID3D12GraphicsCommandList* gLaunchList = nullptr;
ID3D12DescriptorHeap* gLaunchHeap = nullptr;
UINT gLaunchGroups[3]{};

ID3D12GraphicsCommandList* gBoundList = nullptr;

auto* const kFg = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x10000));
auto* const kDeviceA = reinterpret_cast<ID3D12Device*>(static_cast<uintptr_t>(0x20000));
auto* const kDeviceB = reinterpret_cast<ID3D12Device*>(static_cast<uintptr_t>(0x30000));
const xmx::Rvas gRvas{};
char gHandle;

unsigned Count(const char* what)
{
    unsigned count = 0;
    for (const auto& event : gEvents) count += event.what == what;
    return count;
}

const Event* Last(const char* what)
{
    for (auto event = gEvents.rbegin(); event != gEvents.rend(); ++event)
        if (event->what == what) return &*event;
    return nullptr;
}

unsigned Reports(const char* fragment)
{
    unsigned count = 0;
    for (const auto& report : gReports) count += report.find(fragment) != std::string::npos;
    return count;
}

unsigned Logs(const char* fragment)
{
    unsigned count = 0;
    for (const auto& line : gLogs) count += line.find(fragment) != std::string::npos;
    return count;
}

bool CreateOn(ID3D12Device* device)
{
    void* handle = nullptr;
    std::string failure;
    return xmx::tensor_cores::Create(kFg, gRvas, device, handle, failure);
}

int InitializeOne()
{
    xmx::context::InitParams init{};
    return xmx::tensor_cores::Initialize(kFg, gRvas, &gHandle, init);
}

int ExecuteOne()
{
    xmx::context::ExecuteParams execute{};
    return xmx::tensor_cores::Execute(kFg, gRvas, &gHandle, RawList(), execute);
}

int Inactive()
{
    Check(CreateOn(kDeviceA), "a context is created");
    Check(gEvents.size() == 2 && gEvents[0].what == "install" && gEvents[1].what == "create",
        "the route is installed before the first context is created");
    Check(gEvents[0].token == 0 && gEvents[1].token != 0, "creation runs in a pipeline scope, the install outside it");
    const auto token = gEvents[1].token;
    CreateOn(kDeviceA);
    Check(Count("install") == 1, "a second context on the same device does not install again");
    CreateOn(kDeviceB);
    Check(Count("install") == 2, "a context on another device asks for an install of its own");
    Check(Last("create")->token == token, "the pipeline token is the same for every context of the process");
    Check(InitializeOne() == 0 && Last("init")->token == token, "initialization runs in the same pipeline scope");
    Check(ExecuteOne() == 0 && Last("execute")->list == RawList() && Last("execute")->token == 0,
        "while the route is not active the model runs on the game's own list, outside any scope");
    return 0;
}

int Active()
{
    gState = xefg_cuda::State::Active;
    CreateOn(kDeviceA);
    const auto token = Last("create")->token;
    InitializeOne();
    Check(ExecuteOne() == 0, "the model runs");
    Check(Last("execute")->list != RawList() && Last("execute")->token == token,
        "with the route active it records into the proxy list, in the pipeline scope of the contexts");
    Check(Reports("\"state\":\"bound\"") == 1, "the proxy was bound to the game's list");
    Check(gBoundList == RawList(), "a list nothing wraps is bound as it came");
    Check(Reports("disabled") == 0, "a zero result leaves the proxy enabled");
    ExecuteOne();
    Check(Last("execute")->list != RawList(), "the proxy is bound again for the next frame");

    gExecuteResult = 3;
    Check(ExecuteOne() == 3, "a warning run still reports its result");
    Check(Reports("disabled") == 0, "a warning result leaves the proxy enabled");
    gExecuteResult = 0;
    ExecuteOne();
    Check(Last("execute")->list != RawList(), "after a warning the model still records into the proxy list");

    gExecuteResult = -5;
    Check(ExecuteOne() == -5, "a failing run still reports its result");
    Check(Reports("\"state\":\"disabled\",\"result\":-5") == 1, "an error result disables the proxy");
    gExecuteResult = 0;
    ExecuteOne();
    Check(Last("execute")->list == RawList(), "after that the model runs on the game's own list");
    return 0;
}

int Refused()
{
    gState = xefg_cuda::State::Active;
    gCanRecord = false;
    CreateOn(kDeviceA);
    InitializeOne();
    gExecuteResult = -5;
    Check(ExecuteOne() == -5 && Last("execute")->list == RawList(),
        "a list the route cannot record into gets the game's own list");
    Check(Reports("disabled") == 0, "a refused bind is not a failed run: nothing is disabled");
    return 0;
}

int Wrapped()
{
    gState = xefg_cuda::State::Active;
    CreateOn(kDeviceA);
    InitializeOne();
    xmx::context::ExecuteParams execute{};
    Check(xmx::tensor_cores::Execute(kFg, gRvas, &gHandle, WrappedList(), execute) == 0, "the model runs");
    Check(gBoundList == NativeList(), "the route is asked about the list under the overlay's wrapper");
    Check(Last("execute")->list != WrappedList() && Last("execute")->list != NativeList(),
        "the model records into the proxy list, which the overlay's wrapper still sits under");
    gCanRecord = false;
    xmx::tensor_cores::Execute(kFg, gRvas, &gHandle, WrappedList(), execute);
    Check(Last("execute")->list == WrappedList(), "a refused bind runs the model on the game's list as it came");
    return 0;
}

ID3D12PipelineState* const kPipeline = reinterpret_cast<ID3D12PipelineState*>(&gHandle);

void ResetLaunch(bool result)
{
    gLayer = 1;
    gLaunchResult = result;
    gLaunchCalls = 0;
    gLaunchList = nullptr;
    gLaunchHeap = nullptr;
    gLaunchNativeStart = 0;
}

void SetHeaps(xefg_cuda::ProviderList& proxy, std::initializer_list<ID3D12DescriptorHeap*> heaps)
{
    std::vector<ID3D12DescriptorHeap*> list(heaps);
    proxy.Interface()->SetDescriptorHeaps(UINT(list.size()), list.data());
}

int ProxyWrapped()
{
    WrappedList();
    xefg_cuda::ProviderList proxy;
    Check(proxy.Bind(AsList(gWrappedList)), "a list under a wrapper is bound");
    Check(gBoundList == AsList(gNativeList), "the route is asked about the native list");
    Check(Reports("\"state\":\"bound\"") == 1 && Reports("\"wrapped\":true") == 1, "the bound report says the list was unwrapped");
    Check(gWrappedList.refs == 2 && gNativeList.refs == 2, "the proxy holds both lists while it is bound");

    ResetLaunch(true);
    auto* const sampler = AsHeap(gWrappedSampler);
    auto* const view = AsHeap(gWrappedView);
    SetHeaps(proxy, {sampler, view});
    Check(gWrappedList.heapSets == 1 && gWrappedList.heapCount == 2 && gWrappedList.heaps[0] == sampler && gWrappedList.heaps[1] == view,
        "the wrapped list receives the heaps exactly as they were passed");
    Check(gNativeList.heapSets == 0, "the native list receives no heap call");
    Check(gNativeView.refs == 1 && gWrappedView.refs == 1 && gNativeSampler.refs == 1 && gWrappedSampler.refs == 1,
        "the proxy takes no reference on the descriptor heaps");
    proxy.Interface()->SetPipelineState(kPipeline);
    proxy.Interface()->Dispatch(3, 4, 5);
    Check(gLaunchCalls == 1 && gLaunchList == AsList(gNativeList), "a recognized layer is launched on the native list");
    Check(gLaunchGroups[0] == 3 && gLaunchGroups[1] == 4 && gLaunchGroups[2] == 5, "with the dispatch's group counts");
    Check(gLaunchHeap == view,
        "with the heap as it was passed, whose CPU handles the overlay's hook on the runtime's CUDA calls translates");
    Check(gLaunchNativeStart == kNativeViewStart && gLaunchNativeStart != kWrappedViewStart,
        "and the CPU start of the native heap under the wrapper");
    Check(gWrappedList.dispatches == 0 && gNativeList.dispatches == 0, "a launch that succeeded is not recorded as a dispatch");

    gLaunchResult = false;
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchCalls == 2 && gLaunchList == AsList(gNativeList), "a refused launch is tried on the native list as well");
    Check(gWrappedList.dispatches == 1 && gNativeList.dispatches == 0, "and the dispatch goes to the list the proxy was given");

    gLaunchResult = true;
    SetHeaps(proxy, {AsHeap(gPlainView)});
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchHeap == AsHeap(gPlainView), "a heap nothing wraps is the launch's heap itself");
    Check(gLaunchNativeStart == kPlainViewStart, "and its own CPU start is the native one");
    Check(gPlainView.refs == 1, "a heap nothing wraps is not held either");

    SetHeaps(proxy, {sampler});
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchHeap == nullptr && gPlainView.refs == 1, "heaps without a view heap leave the launch without one");
    Check(gLaunchNativeStart == 0, "and without a native CPU start");
    Check(gWrappedList.heapSets == 3 && gWrappedList.heapCount == 1 && gWrappedList.heaps[0] == sampler,
        "and the wrapped list still receives them");

    SetHeaps(proxy, {view});
    proxy.Unbind();
    Check(gWrappedList.refs == 1 && gNativeList.refs == 1, "unbinding releases both lists");
    Check(gNativeView.refs == 1 && gWrappedView.refs == 1, "no descriptor heap reference is left after unbinding");
    const auto dispatches = gWrappedList.dispatches;
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchCalls == 4 && gWrappedList.dispatches == dispatches, "an unbound proxy does nothing");
    return 0;
}

int ProxyPlain()
{
    NativeList();
    xefg_cuda::ProviderList proxy;
    Check(proxy.Bind(AsList(gNativeList)), "a list nothing wraps is bound");
    Check(gBoundList == AsList(gNativeList), "the route is asked about that same list");
    Check(Reports("\"wrapped\":false") == 1, "the bound report says nothing was unwrapped");
    Check(gNativeList.refs == 3, "the one list is held twice, as given and as launch list");

    ResetLaunch(true);
    auto* const sampler = AsHeap(gNativeSampler);
    auto* const view = AsHeap(gNativeView);
    SetHeaps(proxy, {sampler, view});
    Check(gNativeList.heapSets == 1 && gNativeList.heapCount == 2 && gNativeList.heaps[0] == sampler && gNativeList.heaps[1] == view,
        "the list receives the heaps as they were passed");
    proxy.Interface()->SetPipelineState(kPipeline);
    proxy.Interface()->Dispatch(6, 7, 8);
    Check(gLaunchCalls == 1 && gLaunchList == AsList(gNativeList) && gLaunchHeap == view, "the launch gets the same list and the same heap");
    Check(gLaunchNativeStart == kNativeViewStart, "and that heap's own CPU start as the native one");
    Check(gNativeList.dispatches == 0, "a launch that succeeded is not recorded as a dispatch");
    gLaunchResult = false;
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gNativeList.dispatches == 1, "a refused launch is recorded on the same list");

    gLaunchResult = true;
    SetHeaps(proxy, {AsHeap(gOpaqueView)});
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchHeap == AsHeap(gOpaqueView), "a heap that cannot be unwrapped at all is the launch's heap as it came");
    Check(gLaunchNativeStart == kOpaqueViewStart, "and its own CPU start is the native one");
    proxy.Unbind();
    Check(gNativeList.refs == 1, "unbinding releases both references");
    return 0;
}

int ProxyNotUnderWine()
{
    gUnderWine = false;
    WrappedList();
    xefg_cuda::ProviderList proxy;
    Check(proxy.Bind(AsList(gWrappedList)), "a list under a wrapper is bound");
    Check(gBoundList == AsList(gWrappedList), "the route is asked about the list as it came, not the native one");
    Check(Reports("\"state\":\"bound\"") == 1 && Reports("\"wrapped\":false") == 1, "the bound report says nothing was unwrapped");
    Check(gWrappedList.refs == 3 && gNativeList.refs == 1, "the wrapped list is held twice and the native one not at all");

    ResetLaunch(true);
    auto* const view = AsHeap(gWrappedView);
    SetHeaps(proxy, {view});
    proxy.Interface()->SetPipelineState(kPipeline);
    proxy.Interface()->Dispatch(3, 4, 5);
    Check(gLaunchCalls == 1 && gLaunchList == AsList(gWrappedList), "a recognized layer is launched on the list as it came");
    Check(gLaunchHeap == view && gLaunchNativeStart == kWrappedViewStart,
        "with the heap as it was passed and that heap's own CPU start, never the native heap's");
    gLaunchResult = false;
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchCalls == 2 && gLaunchList == AsList(gWrappedList) && gWrappedList.dispatches == 1 && gNativeList.dispatches == 0,
        "a refused launch is tried on that list again and the dispatch goes to it");
    proxy.Unbind();
    Check(gWrappedList.refs == 1 && gNativeList.refs == 1, "unbinding releases both references");
    return 0;
}

int ProxyRefused()
{
    WrappedList();
    xefg_cuda::ProviderList proxy;
    gCanRecord = false;
    Check(!proxy.Bind(AsList(gWrappedList)), "a list the route cannot record into is not bound");
    Check(gBoundList == AsList(gNativeList), "the route was asked about the native list");
    Check(gWrappedList.refs == 1 && gNativeList.refs == 1, "a refused bind keeps no reference");
    ResetLaunch(true);
    proxy.Interface()->Dispatch(1, 1, 1);
    Check(gLaunchCalls == 0 && gWrappedList.dispatches == 0, "a proxy that was never bound does nothing");
    gCanRecord = true;
    proxy.Disable(-5);
    Check(!proxy.Bind(AsList(gWrappedList)), "a disabled proxy is not bound");
    Check(gWrappedList.refs == 1 && gNativeList.refs == 1, "a disabled proxy keeps no reference either");
    return 0;
}

int Off()
{
    xmx::tensor_cores::Configure(false);
    gState = xefg_cuda::State::Active;
    Check(CreateOn(kDeviceA), "a context is created with the route configured off");
    Check(Count("install") == 0, "nothing is installed");
    Check(Last("create")->token == 0, "creation runs outside any pipeline scope");
    InitializeOne();
    Check(Last("init")->token == 0, "so does initialization");
    ExecuteOne();
    Check(Last("execute")->list == RawList() && Last("execute")->token == 0 && Reports("bound") == 0,
        "the model runs on the game's own list even while the route reports active");
    xmx::tensor_cores::Configure(true);
    Check(Logs("applies on the next launch") == 1 && Logs("stay off") == 1,
        "turning it on afterwards is logged once and says it applies on restart");
    CreateOn(kDeviceB);
    Check(Count("install") == 0, "and the route stays off for later contexts");
    xmx::tensor_cores::Configure(false);
    xmx::tensor_cores::Configure(true);
    Check(Logs("applies on the next launch") == 1, "later changes are not logged again");
    return 0;
}

int Restart()
{
    xmx::tensor_cores::Configure(true);
    xmx::tensor_cores::Configure(false);
    xmx::tensor_cores::Configure(true);
    Check(gLogs.empty(), "changes before the first context are free and log nothing");
    gState = xefg_cuda::State::Active;
    CreateOn(kDeviceA);
    InitializeOne();
    xmx::tensor_cores::Configure(true);
    Check(gLogs.empty(), "the value the route was installed with logs nothing");
    xmx::tensor_cores::Configure(false);
    Check(Logs("applies on the next launch") == 1 && Logs("stay on") == 1, "turning it off is logged and says it applies on restart");
    xmx::tensor_cores::Configure(false);
    xmx::tensor_cores::Configure(true);
    xmx::tensor_cores::Configure(false);
    Check(gLogs.size() == 1, "and logged once");
    ExecuteOne();
    Check(Last("execute")->list != RawList(), "the route keeps running until the restart");
    return 0;
}

int InEffectBeforeContext()
{
    Check(xmx::tensor_cores::InEffect(), "before any setting the route is in effect by default");
    xmx::tensor_cores::Configure(false);
    Check(!xmx::tensor_cores::InEffect(), "before any context it follows the configured off");
    xmx::tensor_cores::Configure(true);
    Check(xmx::tensor_cores::InEffect(), "before any context it follows the configured on");
    return 0;
}

int InEffectLatchedOn()
{
    xmx::tensor_cores::Configure(true);
    gState = xefg_cuda::State::Active;
    CreateOn(kDeviceA);
    xmx::tensor_cores::Configure(false);
    Check(xmx::tensor_cores::InEffect(), "a route latched on stays in effect after the setting is turned off");
    return 0;
}

int InEffectLatchedOff()
{
    xmx::tensor_cores::Configure(false);
    CreateOn(kDeviceA);
    xmx::tensor_cores::Configure(true);
    Check(!xmx::tensor_cores::InEffect(), "a route latched off stays out of effect after the setting is turned on");
    return 0;
}

int Summary()
{
    using xmx::tensor_cores::Setting;
    using xmx::tensor_cores::StatusText;
    xefg_cuda::Status status;
    Check(StatusText(status, Setting::Undecided) == "tensor cores not attempted (no context yet)",
        "before any context the route is not attempted");
    Check(StatusText(status, Setting::On) == "tensor cores not attempted (route not installed)",
        "after a context without an install it says so");
    status.state = xefg_cuda::State::Active;
    status.launches = 12;
    status.fallbacks = 3;
    Check(StatusText(status, Setting::On) == "tensor cores active launches 12 fallbacks 3",
        "an active route reports its launches and fallbacks");
    status.state = xefg_cuda::State::Disabled;
    status.reason = "optiscaler_ini_off";
    Check(StatusText(status, Setting::On) == "tensor cores off (optiscaler_ini_off)", "a disabled route is off with its reason");
    status.state = xefg_cuda::State::Unavailable;
    status.reason = "no_tensor_cores";
    Check(StatusText(status, Setting::On) == "tensor cores unavailable (no_tensor_cores)",
        "an unavailable route says so with its reason");
    status.state = xefg_cuda::State::Active;
    Check(StatusText(status, Setting::Off) == "tensor cores off (xmxTensorCores 0)",
        "configured off wins over whatever the route reports");

    gStatus = {};
    gStatus.state = xefg_cuda::State::Active;
    gStatus.launches = 7;
    Check(xmx::tensor_cores::SummaryText() == "tensor cores active launches 7 fallbacks 0",
        "the summary reads the route's snapshot");
    xmx::tensor_cores::Configure(false);
    CreateOn(kDeviceA);
    Check(xmx::tensor_cores::SummaryText() == "tensor cores off (xmxTensorCores 0)",
        "once a context was made with the route off the summary says off");
    return 0;
}
}

namespace xmx
{
void Log(const std::string& text) noexcept
{
    gLogs.push_back(text);
    std::printf("log: %s\n", text.c_str());
}
}

namespace xess_mod
{
bool InstallMultiFrameTensorCores(HMODULE fg, ID3D12Device* device) noexcept
{
    ++gInstallCalls;
    gEvents.push_back({"install", gToken, device});
    return fg == kFg && gState == xefg_cuda::State::Active;
}
}

namespace xefg_cuda
{
uint64_t NewPipelineToken() noexcept
{
    return ++gNextToken;
}

bool UnderWine() noexcept
{
    return gUnderWine;
}

PipelineScope::PipelineScope(uint64_t token) noexcept : previous_(gToken)
{
    gToken = token;
}

PipelineScope::~PipelineScope()
{
    gToken = previous_;
}

DeclinedScope::DeclinedScope() noexcept : previous_(false)
{
}

DeclinedScope::~DeclinedScope()
{
}

Status Snapshot() noexcept
{
    Status status = gStatus;
    if (status.state == State::NotAttempted) status.state = gState;
    return status;
}

void Report(const char*, const std::string& fields) noexcept
{
    gReports.push_back(fields);
}

bool CanRecord(ID3D12GraphicsCommandList* list, std::string& reason) noexcept
{
    gBoundList = list;
    reason = "fake";
    return gCanRecord;
}

uint8_t LayerOf(ID3D12PipelineState* state) noexcept
{
    return state ? gLayer : 0;
}

bool LaunchOn(ID3D12GraphicsCommandList* list, const ListState& state, UINT x, UINT y, UINT z) noexcept
{
    ++gLaunchCalls;
    gLaunchList = list;
    gLaunchHeap = state.heap;
    gLaunchNativeStart = state.nativeCpuStart.ptr;
    gLaunchGroups[0] = x;
    gLaunchGroups[1] = y;
    gLaunchGroups[2] = z;
    return gLaunchResult;
}
}

namespace xmx::context
{
bool Create(HMODULE, const Rvas&, ID3D12Device*, void*& handle, std::string&) noexcept
{
    handle = &gHandle;
    gEvents.push_back({"create", gToken, nullptr});
    return true;
}

int Initialize(HMODULE, const Rvas&, void*, InitParams&) noexcept
{
    gEvents.push_back({"init", gToken, nullptr});
    return 0;
}

int Execute(HMODULE, const Rvas&, void*, ID3D12GraphicsCommandList* list, ExecuteParams&) noexcept
{
    gEvents.push_back({"execute", gToken, list});
    return gExecuteResult;
}
}

int wmain(int argc, wchar_t** argv)
{
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    int code = 1;
    if (mode == L"inactive") code = Inactive();
    else if (mode == L"active") code = Active();
    else if (mode == L"refused") code = Refused();
    else if (mode == L"wrapped") code = Wrapped();
    else if (mode == L"proxy_wrapped") code = ProxyWrapped();
    else if (mode == L"proxy_plain") code = ProxyPlain();
    else if (mode == L"proxy_not_under_wine") code = ProxyNotUnderWine();
    else if (mode == L"proxy_refused") code = ProxyRefused();
    else if (mode == L"off") code = Off();
    else if (mode == L"restart") code = Restart();
    else if (mode == L"summary") code = Summary();
    else if (mode == L"in_effect_before_context") code = InEffectBeforeContext();
    else if (mode == L"in_effect_latched_on") code = InEffectLatchedOn();
    else if (mode == L"in_effect_latched_off") code = InEffectLatchedOff();
    else std::printf("FAIL unknown mode\n");
    std::printf("XMX_TENSOR_CORES checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures || code ? 1 : 0;
}
