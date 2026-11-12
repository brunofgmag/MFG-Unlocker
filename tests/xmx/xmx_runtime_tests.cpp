#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "single_module.h"
#include "xess/runtime_layout.h"
#include "xess/xess_mod.h"
#include "xmx/xmx_context.h"
#include "xmx/xmx_rules.h"

using Microsoft::WRL::ComPtr;

namespace
{
HMODULE gPayload = nullptr;
unsigned gChecks = 0, gFailures = 0;

bool Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    return ok;
}

bool IsFunctionStart(HMODULE module, uintptr_t rva)
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto* entries = reinterpret_cast<const RUNTIME_FUNCTION*>(base + directory.VirtualAddress);
    const size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);
    for (size_t index = 0; index < count; ++index)
        if (entries[index].BeginAddress == rva) return true;
    return false;
}

bool VtableLooksRight(HMODULE module, uintptr_t rva)
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* slots = reinterpret_cast<const uintptr_t*>(base + rva);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section)
    {
        if (rva < section->VirtualAddress || rva >= section->VirtualAddress + section->Misc.VirtualSize) continue;
        if (std::memcmp(section->Name, ".rdata", 6) != 0) return false;
        for (int slot = 0; slot < 8; ++slot)
        {
            const auto target = slots[slot] - base;
            if (slots[slot] < base || target >= nt->OptionalHeader.SizeOfImage) return false;
            if (*reinterpret_cast<const unsigned char*>(slots[slot]) != 0xE9) return false;
        }
        return true;
    }
    return false;
}

std::string Hex(HRESULT result)
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
    return text;
}

ComPtr<ID3D12Device> HardwareDevice(std::string& why)
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        why = "CreateDXGIFactory1 returned " + Hex(result);
        return nullptr;
    }
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::printf("adapter %u: vendor 0x%04X device 0x%04X flags 0x%X\n", index, desc.VendorId, desc.DeviceId,
            desc.Flags);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device> device;
        result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        std::printf("D3D12CreateDevice on adapter %u at feature level 12_0 returned %s\n", index, Hex(result).c_str());
        if (SUCCEEDED(result)) return device;
        why = "D3D12CreateDevice at feature level 12_0 returned " + Hex(result) + " on every hardware adapter";
    }
    if (why.empty()) why = "DXGI lists no hardware adapter";
    return nullptr;
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
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, UINT16 mips = 1)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    return texture;
}

void PrintMemory(ID3D12Device* device, const char* moment)
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))
        && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        std::printf("memory %s: used %llu MB, budget %llu MB\n", moment,
            static_cast<unsigned long long>(info.CurrentUsage >> 20), static_cast<unsigned long long>(info.Budget >> 20));
    std::fflush(stdout);
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&buffer));
    return buffer;
}

ComPtr<ID3D12Resource> UploadOf(ID3D12Device* device, const std::vector<uint32_t>& texels)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = texels.size() * sizeof(uint32_t);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    void* mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&buffer)))
        || FAILED(buffer->Map(0, nullptr, &mapped)))
        return nullptr;
    std::memcpy(mapped, texels.data(), desc.Width);
    buffer->Unmap(0, nullptr);
    return buffer;
}

void Fill(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* upload, UINT width, UINT height,
    DXGI_FORMAT format, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    to.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION from{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    from.PlacedFootprint.Footprint = {format, width, height, 1, width * 4};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
}

std::vector<uint32_t> Scene(UINT width, UINT height, int shift)
{
    std::vector<uint32_t> texels(size_t{width} * height);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const UINT u = x + shift, r = (u * 7 + y * 3) & 1023, g = ((u ^ y) * 5) & 1023, b = ((u / 64 + y / 64) & 1) * 900;
            texels[size_t{y} * width + x] = r | (g << 10) | (b << 20) | (3u << 30);
        }
    return texels;
}

xmx::context::Input InputOf(ID3D12Resource* resource, UINT width, UINT height)
{
    return {resource, 0, 0, width, height};
}

int RunExecute(HMODULE fg, const xmx::Rvas& rvas, unsigned idleFrames, unsigned idleSeconds, bool hudless,
    UINT16 depthMips, unsigned loadMb, bool runModel, bool content, unsigned resetFrame)
{
    constexpr UINT kWidth = 3840, kHeight = 2160, kFrames = 3;
    std::string why;
    Gpu gpu;
    gpu.device = HardwareDevice(why);
    if (!gpu.device || !MakeGpu(gpu))
    {
        std::printf("SETUP no D3D12 hardware device: %s\n", why.c_str());
        return 2;
    }
    const auto input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ComPtr<ID3D12Resource> color[2], depth[2], motion[2], hud[2], outputs[kFrames];
    for (int i = 0; i < 2; ++i)
    {
        color[i] = MakeTexture(gpu.device.Get(), kWidth, kHeight, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_NONE, input);
        depth[i] = MakeTexture(gpu.device.Get(), 1920, 1080, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, input,
            depthMips);
        motion[i] = MakeTexture(gpu.device.Get(), 1920, 1080, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, input);
        hud[i] = MakeTexture(gpu.device.Get(), kWidth, kHeight, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_NONE, input);
    }
    for (auto& output : outputs)
        output = MakeTexture(gpu.device.Get(), kWidth, kHeight, DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::vector<ComPtr<ID3D12Resource>> uploads;
    if (content)
    {
        const uint16_t half = 0x3800;
        std::vector<uint32_t> depthTexels(size_t{1920} * 1080, 0x3F000000u);
        std::vector<uint32_t> motionTexels(size_t{1920} * 1080, uint32_t{half} | (uint32_t{0} << 16));
        for (int i = 0; i < 2; ++i)
        {
            uploads.push_back(UploadOf(gpu.device.Get(), Scene(kWidth, kHeight, i * 8)));
            Fill(gpu.list.Get(), color[i].Get(), uploads.back().Get(), kWidth, kHeight, DXGI_FORMAT_R10G10B10A2_UNORM, input);
            Fill(gpu.list.Get(), hud[i].Get(), uploads.back().Get(), kWidth, kHeight, DXGI_FORMAT_R10G10B10A2_UNORM, input);
        }
        uploads.push_back(UploadOf(gpu.device.Get(), depthTexels));
        uploads.push_back(UploadOf(gpu.device.Get(), motionTexels));
        for (int i = 0; i < 2; ++i)
        {
            Fill(gpu.list.Get(), depth[i].Get(), uploads[2].Get(), 1920, 1080, DXGI_FORMAT_R32_FLOAT, input);
            Fill(gpu.list.Get(), motion[i].Get(), uploads[3].Get(), 1920, 1080, DXGI_FORMAT_R16G16_FLOAT, input);
        }
        if (!Submit(gpu)) return Check(false, "the content upload completes"), 1;
    }
    PrintMemory(gpu.device.Get(), "before the context");
    void* handle = nullptr;
    std::string failure;
    if (!Check(xmx::context::Create(fg, rvas, gpu.device.Get(), handle, failure) && handle, "the factory makes a context"))
        return 1;
    xmx::context::InitParams init{};
    init.width = kWidth;
    init.height = kHeight;
    init.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    init.flags = xmx::context::kFlagDepthInverted;
    init.maxFrames = kFrames;
    init.creationMask = 1;
    init.visibleMask = 1;
    if (!Check(xmx::context::Initialize(fg, rvas, handle, init) == 0, "Init accepts the 4K context")) return 1;
    PrintMemory(gpu.device.Get(), "after Init");
    for (unsigned frame = 0; frame < idleFrames; ++frame)
        if (!Submit(gpu)) return Check(false, "an idle frame completes"), 1;
    if (idleSeconds) Sleep(idleSeconds * 1000);
    PrintMemory(gpu.device.Get(), "after the idle frames");
    ID3D12Resource* outputList[kFrames]{outputs[0].Get(), outputs[1].Get(), outputs[2].Get()};
    const uint32_t bases[kFrames * 2]{};
    int crashed = 0, failed = 0;
    constexpr UINT64 kChunk = 256ull << 20;
    std::vector<ComPtr<ID3D12Resource>> load;
    const auto source = loadMb ? MakeBuffer(gpu.device.Get(), kChunk) : nullptr;
    const unsigned chunks = loadMb / 256;
    const unsigned frames = 60 + chunks * 10 + 300;
    const ULONGLONG started = GetTickCount64();
    for (unsigned frame = 0; frame < frames; ++frame)
    {
        const int next = frame & 1, previous = next ^ 1;
        xmx::context::ExecuteParams execute{};
        for (auto* data : {&execute.previous, &execute.next})
        {
            const int slot = data == &execute.next ? next : previous;
            data->backbuffer = InputOf(color[slot].Get(), kWidth, kHeight);
            data->depth = InputOf(depth[slot].Get(), 1920, 1080);
            data->motion = InputOf(motion[slot].Get(), 1920, 1080);
            if (hudless) data->hudless = InputOf(hud[slot].Get(), kWidth, kHeight);
            for (int i = 0; i < 4; ++i) data->projection[i * 5] = 1.f;
            data->resolution[0] = kWidth;
            data->resolution[1] = kHeight;
            data->motionScale[0] = data->motionScale[1] = 1.f;
        }
        if (resetFrame && frame == resetFrame)
        {
            execute.previous.reset = 1;
            PrintMemory(gpu.device.Get(), "before the reset execute");
        }
        execute.outputs = outputList;
        execute.outputBases = bases;
        execute.outputCount = kFrames;
        execute.firstFrame = frame % kFrames;
        execute.frameCount = 1;
        if (frame % 10 == 0 && load.size() < chunks)
        {
            load.push_back(MakeBuffer(gpu.device.Get(), kChunk));
            if (load.back()) gpu.list->CopyBufferRegion(load.back().Get(), 0, source.Get(), 0, kChunk);
        }
        if (frame % 60 == 0 && frame) PrintMemory(gpu.device.Get(), ("frame " + std::to_string(frame)).c_str());
        const int result = runModel ? xmx::context::Execute(fg, rvas, handle, gpu.list.Get(), execute) : 0;
        if (frame < 3 || result != 0) std::printf("execute %u result %d\n", frame, result);
        crashed += result == xmx::context::kCrashed;
        failed += result != 0;
        if (result == xmx::context::kCrashed) break;
        if (!Submit(gpu)) return Check(false, "an executed frame completes"), 1;
        if (frame == 0) PrintMemory(gpu.device.Get(), "after the first execute");
        if (resetFrame && frame == resetFrame)
        {
            PrintMemory(gpu.device.Get(), "after the reset execute");
            Sleep(3000);
        }
    }
    std::printf("%u frames in %llu ms\n", frames, static_cast<unsigned long long>(GetTickCount64() - started));
    PrintMemory(gpu.device.Get(), "at the end");
    std::printf("device removed reason %s\n", Hex(gpu.device->GetDeviceRemovedReason()).c_str());
    Check(crashed == 0, "XeFG never crashes");
    Check(failed == 0, "every execute returns 0");
    return gFailures ? 1 : 0;
}

int RunOnDevice(HMODULE fg, const xmx::Rvas& rvas)
{
    std::string why;
    const ComPtr<ID3D12Device> device = HardwareDevice(why);
    if (!device)
    {
        std::printf("SETUP no D3D12 hardware device at feature level 12_0: %s\n", why.c_str());
        return 2;
    }
    void* handle = nullptr;
    std::string failure;
    if (!Check(xmx::context::Create(fg, rvas, device.Get(), handle, failure) && handle,
            "the factory at its RVA returns a context with the expected vtable"))
    {
        std::printf("factory: %s\n", failure.c_str());
        return 1;
    }
    xmx::context::InitParams init{};
    init.width = 1920;
    init.height = 1080;
    init.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    init.maxFrames = xmx::kMaxFrames;
    init.creationMask = 1;
    init.visibleMask = 1;
    const int result = xmx::context::Initialize(fg, rvas, handle, init);
    std::printf("init result %d\n", result);
    Check(result == 0, "Init at its RVA accepts maxFrames 5 with the published model");
    return gFailures ? 1 : 0;
}
}

namespace single_module
{
void Log(const wchar_t* text) noexcept
{
    std::wprintf(L"log: %ls\n", text);
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
    if (argc < 4) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring folder = argv[1];
    const std::wstring mode = argv[3];
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

    Check(IsFunctionStart(fg, rvas->factory) && IsFunctionStart(fg, rvas->init) && IsFunctionStart(fg, rvas->execute),
        "the factory, Init and Execute RVAs are function entries of this build");
    Check(VtableLooksRight(fg, rvas->contextVtable), "the context vtable RVA is a table of code thunks in .rdata");

    std::string error;
    const bool published = xess_mod::PublishMultiFrameModel(fg, ll, error);
    if (!Check(published, "the multi-frame model publishes through xess_mod"))
        std::printf("publish error: %s\n", error.c_str());
    Check(xess_mod::PublishMultiFrameModel(fg, ll, error), "a second publication on the same pair is a no-op");

    int outcome = published && mode == L"device" ? RunOnDevice(fg, *rvas) : 0;
    if (published && mode == L"execute")
        outcome = RunExecute(fg, *rvas, argc > 4 ? _wtoi(argv[4]) : 0, argc > 5 ? _wtoi(argv[5]) : 0,
            argc > 6 && _wtoi(argv[6]) != 0, static_cast<UINT16>(argc > 7 ? _wtoi(argv[7]) : 1),
            argc > 8 ? _wtoi(argv[8]) : 0, argc > 9 ? _wtoi(argv[9]) != 0 : true,
            argc > 10 && _wtoi(argv[10]) != 0, argc > 11 ? _wtoi(argv[11]) : 0);
    std::printf("XMX_RUNTIME checks=%u failures=%u\n", gChecks, gFailures);
    if (outcome == 2) return 2;
    return gFailures || !published || outcome ? 1 : 0;
}
