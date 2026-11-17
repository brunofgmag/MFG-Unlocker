#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "optiscaler_host.h"
#include "single_module.h"
#include "xess/cuda/cuda_route.h"
#include "xess/runtime_layout.h"
#include "xess/xess_mod.h"
#include "xmx/xmx_context.h"
#include "xmx/xmx_rules.h"
#include "xmx/xmx_tensor_cores.h"

using Microsoft::WRL::ComPtr;

namespace
{
HMODULE gPayload = nullptr;
unsigned gChecks = 0, gFailures = 0;

constexpr unsigned kContexts = 16;
constexpr UINT kFrames = 3;
constexpr UINT kBaseWidth = 640, kBaseHeight = 360, kStepWidth = 64, kStepHeight = 36;
constexpr unsigned kRunFrames = 4;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

ComPtr<ID3D12Device> HardwareDevice()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) return nullptr;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) return device;
    }
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;
};

bool MakeGpu(Gpu& gpu)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return SUCCEEDED(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&gpu.queue)))
        && SUCCEEDED(gpu.device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&gpu.allocator)))
        && SUCCEEDED(gpu.device->CreateCommandList(0, desc.Type, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)))
        && SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)));
}

bool Submit(Gpu& gpu)
{
    if (FAILED(gpu.list->Close())) return false;
    ID3D12CommandList* lists[] = {gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, lists);
    gpu.queue->Signal(gpu.fence.Get(), ++gpu.value);
    gpu.fence->SetEventOnCompletion(gpu.value, gpu.event);
    WaitForSingleObject(gpu.event, 10000);
    return gpu.fence->GetCompletedValue() >= gpu.value && SUCCEEDED(gpu.allocator->Reset())
        && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr));
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    return texture;
}

xmx::context::Input InputOf(ID3D12Resource* resource, UINT width, UINT height)
{
    return {resource, 0, 0, width, height};
}

struct Shape
{
    UINT width, height;
    ComPtr<ID3D12Resource> color[2], depth[2], motion[2], outputs[kFrames];
};

bool MakeShape(ID3D12Device* device, Shape& shape)
{
    const auto input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i)
    {
        shape.color[i] = MakeTexture(device, shape.width, shape.height, DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D12_RESOURCE_FLAG_NONE, input);
        shape.depth[i] = MakeTexture(device, shape.width / 2, shape.height / 2, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, input);
        shape.motion[i] = MakeTexture(device, shape.width / 2, shape.height / 2, DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, input);
    }
    for (auto& output : shape.outputs)
        output = MakeTexture(device, shape.width, shape.height, DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return shape.color[0] && shape.color[1] && shape.depth[0] && shape.depth[1] && shape.motion[0]
        && shape.motion[1] && shape.outputs[0] && shape.outputs[1] && shape.outputs[2];
}

xmx::context::ExecuteParams ExecuteOf(const Shape& shape, unsigned frame, ID3D12Resource** outputList)
{
    static const uint32_t bases[kFrames * 2]{};
    const int next = frame & 1, previous = next ^ 1;
    xmx::context::ExecuteParams execute{};
    for (auto* data : {&execute.previous, &execute.next})
    {
        const int slot = data == &execute.next ? next : previous;
        data->backbuffer = InputOf(shape.color[slot].Get(), shape.width, shape.height);
        data->depth = InputOf(shape.depth[slot].Get(), shape.width / 2, shape.height / 2);
        data->motion = InputOf(shape.motion[slot].Get(), shape.width / 2, shape.height / 2);
        for (int i = 0; i < 4; ++i) data->projection[i * 5] = 1.f;
        data->resolution[0] = static_cast<float>(shape.width);
        data->resolution[1] = static_cast<float>(shape.height);
        data->motionScale[0] = data->motionScale[1] = 1.f;
    }
    execute.outputs = outputList;
    execute.outputBases = bases;
    execute.outputCount = kFrames;
    execute.firstFrame = frame % kFrames;
    execute.frameCount = 1;
    return execute;
}

struct Outcome
{
    bool ok = false;
    uint32_t layers = 0;
    uint64_t launchesGained = 0;
};

Outcome RunContext(Gpu& gpu, HMODULE fg, const xmx::Rvas& rvas, Shape& shape)
{
    Outcome outcome;
    void* handle = nullptr;
    std::string failure;
    if (!xmx::tensor_cores::Create(fg, rvas, gpu.device.Get(), handle, failure) || !handle) return outcome;
    xmx::context::InitParams init{};
    init.width = shape.width;
    init.height = shape.height;
    init.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    init.flags = xmx::context::kFlagDepthInverted;
    init.maxFrames = kFrames;
    init.creationMask = 1;
    init.visibleMask = 1;
    if (xmx::tensor_cores::Initialize(fg, rvas, handle, init) != 0) return outcome;
    outcome.layers = xefg_cuda::Snapshot().layers;
    ID3D12Resource* outputList[kFrames]{shape.outputs[0].Get(), shape.outputs[1].Get(), shape.outputs[2].Get()};
    const uint64_t before = xefg_cuda::Snapshot().launches;
    for (unsigned frame = 0; frame < kRunFrames; ++frame)
    {
        auto execute = ExecuteOf(shape, frame, outputList);
        if (xmx::tensor_cores::Execute(fg, rvas, handle, gpu.list.Get(), execute) != 0) return outcome;
        if (!Submit(gpu)) return outcome;
    }
    outcome.launchesGained = xefg_cuda::Snapshot().launches - before;
    outcome.ok = true;
    return outcome;
}

int Contexts(HMODULE fg, HMODULE ll, const xmx::Rvas& rvas)
{
    Gpu gpu;
    gpu.device = HardwareDevice();
    if (!gpu.device || !MakeGpu(gpu))
    {
        std::printf("SETUP no D3D12 hardware device at feature level 12_0\n");
        return 2;
    }
    std::string error;
    if (!Check(xess_mod::PublishMultiFrameModel(fg, ll, error), "the multi-frame model publishes")) return 1;
    std::vector<Shape> shapes(kContexts);
    std::vector<Outcome> outcomes(kContexts);
    for (unsigned i = 0; i < kContexts; ++i)
    {
        shapes[i].width = kBaseWidth + i * kStepWidth;
        shapes[i].height = kBaseHeight + i * kStepHeight;
        if (!Check(MakeShape(gpu.device.Get(), shapes[i]), "the shape's textures are made")) return 1;
        outcomes[i] = RunContext(gpu, fg, rvas, shapes[i]);
        std::printf("context %u %ux%u: ok %d, recognized layers %u, launches gained %llu\n", i + 1, shapes[i].width,
            shapes[i].height, outcomes[i].ok ? 1 : 0, outcomes[i].layers,
            static_cast<unsigned long long>(outcomes[i].launchesGained));
        std::fflush(stdout);
        if (!Check(outcomes[i].ok, "the context is created, initialized and executed")) return 1;
    }
    const auto status = xefg_cuda::Snapshot();
    std::printf("route state %d, layers %u, launches %llu, fallbacks %llu, reason %s\n",
        static_cast<int>(status.state), status.layers, static_cast<unsigned long long>(status.launches),
        static_cast<unsigned long long>(status.fallbacks), status.reason.c_str());
    Check(status.state == xefg_cuda::State::Active, "the route is active");
    Check(outcomes.front().launchesGained > 0, "the first context's execute launched on the tensor cores");
    Check(outcomes.back().launchesGained > 0, "the sixteenth context's execute launched on the tensor cores");
    return gFailures ? 1 : 0;
}
}

namespace xmx
{
void Log(const std::string& text) noexcept
{
    std::printf("log: %s\n", text.c_str());
}
}

namespace single_module
{
void Log(const wchar_t* text) noexcept
{
    std::wprintf(L"log: %ls\n", text);
}
}

namespace optiscaler_host
{
bool ReadXefgTensorCores(std::wstring&) noexcept
{
    return true;
}

uint32_t ReadXefgTensorCoresMask(std::wstring&) noexcept
{
    return 0x1FFFFu;
}
}

namespace xess_mod
{
HMODULE Self() noexcept
{
    return gPayload;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) return 1;
    wchar_t gate[8]{};
    if (GetEnvironmentVariableW(L"MFG_TEST_GPU", gate, 8) == 0 || gate[0] != L'1')
    {
        std::printf("SETUP creates a GPU device: set MFG_TEST_GPU=1 with the game closed\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring folder = argv[1];
    gPayload = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const HMODULE ll = LoadLibraryExW((folder + L"\\libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const HMODULE fg = LoadLibraryExW((folder + L"\\libxess_fg.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Check(gPayload && ll && fg, "the payload module, libxell.dll and libxess_fg.dll load")) return 1;
    std::string family;
    try
    {
        family = xess_layout::Get(fg).family;
    }
    catch (const std::exception& error)
    {
        std::printf("layout: %s\n", error.what());
    }
    const xmx::Rvas* rvas = xmx::RvasFor(family);
    if (!Check(rvas != nullptr, "libxess_fg.dll 1.3.1.78 is the supported fg78 family")) return 1;
    const int outcome = Contexts(fg, ll, *rvas);
    std::printf("XMX_TENSOR_CORE_CONTEXTS checks=%u failures=%u\n", gChecks, gFailures);
    return outcome;
}
