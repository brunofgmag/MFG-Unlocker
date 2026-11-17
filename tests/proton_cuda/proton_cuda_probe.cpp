#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>
#include "overlay_native.h"
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
using NvQuery = void*(__cdecl*)(uint32_t);
using NvInitialize = int(__cdecl*)();
using NvCreate = int(__cdecl*)(ID3D12Device*, const void*, uint32_t, uint32_t, uint32_t, uint32_t, const char*, void**);
using NvLaunch = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
using NvDestroy = int(__cdecl*)(ID3D12Device*, void*);
struct DescriptorObject
{
    size_t structSizeIn, structSizeOut;
    ID3D12Device* device;
    int type;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
    uint64_t handle;
};
static_assert(sizeof(DescriptorObject) == 48 && offsetof(DescriptorObject, descriptor) == 32);
using NvDescriptorObject = int(__cdecl*)(DescriptorObject*);
constexpr int kSurfaceObject = 0, kTextureObject = 1;
constexpr uint32_t kIdInitialize = 0x0150e828, kIdCreate = 0x1dc7261f, kIdLaunch = 0x5c52bb86,
    kIdDestroy = 0x7fb785ba, kIdDescriptorObject = 0x0ddac234;
constexpr int kFault = 0x7FFFFFFF;
constexpr GUID kUnwrappedObject = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

struct Nvapi
{
    NvInitialize initialize{};
    NvCreate create{};
    NvLaunch launch{};
    NvDestroy destroy{};
    NvDescriptorObject object{};
};

struct RawParams { uint64_t input, output; uint32_t count, pad; };
struct ImageParams { uint64_t texture, surface; uint32_t width, height, layers, pad; };
struct TypedParams { uint64_t texture, output; uint32_t count, pad; };

constexpr uint32_t kElements = 256, kBytes = kElements * 16, kInOffset = 256, kOutOffset = 512;
constexpr uint32_t kInSize = kInOffset + kBytes, kOutSize = kOutOffset + kBytes + 256;
constexpr uint32_t kFirstElement = kInOffset / 16;
constexpr uint32_t kBigDescriptors = 100000, kRouteSrvSlot = 32, kRouteUavSlot = 33, kEmptySlot = 40;
constexpr uint32_t kWidth = 16, kHeight = 8, kLayers = 2;
constexpr uint32_t kSentinel = 0xCDCDCDCDu;
constexpr uint32_t kUploadIn = 0, kUploadOut = 8192, kUploadOut2 = 16384, kUploadTexture = 24576;
constexpr uint32_t kReadbackOut = 0, kReadbackOut2 = 8192, kReadbackTexture = 16384;

struct Uint4 { uint32_t x, y, z, w; };

FILE* gOut = nullptr;
bool gAllPassed = true;

void Say(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    if (gOut)
    {
        fprintf(gOut, "%s\n", line);
        fflush(gOut);
    }
}

void Result(const char* id, const char* what, int code, uint64_t handle, bool pass, const char* detail)
{
    gAllPassed = gAllPassed && pass;
    Say("RESULT %-4s %-46s code=%-11d handle=0x%016llx %s %s", id, what, code, static_cast<unsigned long long>(handle),
        pass ? "PASS" : "FAIL", detail);
}

void Skipped(const char* id, const char* what, const char* reason)
{
    gAllPassed = false;
    Say("RESULT %-4s %-46s SKIP %s", id, what, reason);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    Say("CRASH exception=0x%08lx address=%p", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

int GuardedCreate(NvCreate create, ID3D12Device* device, const void* data, uint32_t size, const char* name, void** handle) noexcept
{
    __try { return create(device, data, size, 128, 1, 1, name, handle); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kFault; }
}

int GuardedLaunch(NvLaunch launch, ID3D12GraphicsCommandList* list, void* kernel, uint32_t x, uint32_t y, uint32_t z,
                  const void* params, uint32_t size) noexcept
{
    __try { return launch(list, kernel, x, y, z, params, size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kFault; }
}

int GuardedObject(NvDescriptorObject object, DescriptorObject* request) noexcept
{
    __try { return object(request); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return kFault; }
}

std::wstring ModulePath(const void* address)
{
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module))
        return L"(unknown)";
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    return path;
}

std::string Narrow(const std::wstring& text)
{
    std::string out;
    for (wchar_t c : text) out.push_back(c < 128 ? char(c) : '?');
    return out;
}

std::string ObjectModule(IUnknown* object)
{
    if (!object) return "(null)";
    const void* table = *reinterpret_cast<void* const*>(object);
    return Narrow(ModulePath(*reinterpret_cast<void* const*>(table)));
}

ComPtr<ID3D12DescriptorHeap> UnwrapHeap(ID3D12DescriptorHeap* heap)
{
    ComPtr<IUnknown> original;
    if (heap->QueryInterface(kUnwrappedObject, reinterpret_cast<void**>(original.GetAddressOf())) != S_OK || !original) return {};
    ComPtr<ID3D12DescriptorHeap> unwrapped;
    original.As(&unwrapped);
    return unwrapped;
}

std::string UnwrappedStart(ID3D12DescriptorHeap* heap)
{
    const ComPtr<ID3D12DescriptorHeap> unwrapped = UnwrapHeap(heap);
    if (!unwrapped) return "none";
    char text[32];
    snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(unwrapped->GetCPUDescriptorHandleForHeapStart().ptr));
    return text;
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    return text.substr(0, text.find_last_of(L"\\/") + 1);
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& bytes)
{
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    bytes.resize(size_t(ftell(file)));
    fseek(file, 0, SEEK_SET);
    const size_t read = fread(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    return read == bytes.size();
}

Uint4 InputElement(uint32_t g)
{
    return {g * 2654435761u ^ 0x1111u, g + 0x100u, g * 7u + 3u, ~g};
}

Uint4 InputTexel(uint32_t g)
{
    return {g * 2246822519u ^ 0x2222u, g * 3u + 0x77u, g ^ 0xDEADBEEFu, g + 0x9999u};
}

Uint4 Transform(Uint4 v)
{
    return {v.x + 1u, v.y ^ 0xA5A5A5A5u, v.z * 3u, v.w - 5u};
}

Uint4 TransformImage(Uint4 v)
{
    return {v.x + 0x1000u, v.y ^ 0xFFFF0000u, v.z * 5u, v.w + 9u};
}

bool Equal(const Uint4& a, const Uint4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

std::string Format(const Uint4& v)
{
    char text[80];
    snprintf(text, sizeof(text), "{%08x %08x %08x %08x}", v.x, v.y, v.z, v.w);
    return text;
}

struct Mismatches
{
    uint32_t count{};
    std::string first;

    void Note(const std::string& where, const Uint4& got, const Uint4& expected)
    {
        if (!count++) first = "first mismatch at " + where + ": got " + Format(got) + " expected " + Format(expected);
    }
};

void Report(const char* id, const char* what, uint64_t handle, const Mismatches& found, const char* exact)
{
    const std::string detail = found.count ? std::to_string(found.count) + " mismatches; " + found.first : exact;
    Result(id, what, 0, handle, found.count == 0, detail.c_str());
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue{};
    HANDLE event{};

    bool Wait()
    {
        queue->Signal(fence.Get(), ++fenceValue);
        if (fence->GetCompletedValue() >= fenceValue) return true;
        fence->SetEventOnCompletion(fenceValue, event);
        return WaitForSingleObject(event, 30000) == WAIT_OBJECT_0;
    }

    ComPtr<ID3D12GraphicsCommandList> NewList()
    {
        allocator->Reset();
        ComPtr<ID3D12GraphicsCommandList> list;
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        return list;
    }

    bool Submit(ID3D12GraphicsCommandList* list)
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        return Wait();
    }

    ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> resource;
        device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource));
        return resource;
    }
};

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER UavBarrier()
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    return barrier;
}

bool LoadNvapi(Nvapi& api)
{
    HMODULE dispatcher = LoadLibraryW(L"nvapi64.dll");
    if (!dispatcher) { Say("SETUP nvapi64.dll not loadable, error=%lu", GetLastError()); return false; }
    Say("SETUP nvapi64.dll=%s", Narrow(ModulePath(dispatcher)).c_str());
    const auto query = reinterpret_cast<NvQuery>(GetProcAddress(dispatcher, "nvapi_QueryInterface"));
    if (!query) { Say("SETUP nvapi_QueryInterface missing"); return false; }
    const std::array<uint32_t, 5> ids{kIdInitialize, kIdCreate, kIdLaunch, kIdDestroy, kIdDescriptorObject};
    std::array<void*, 5> targets{};
    for (size_t i = 0; i < ids.size(); ++i)
    {
        targets[i] = query(ids[i]);
        Say("SETUP interface 0x%08x -> %p in %s", ids[i], targets[i], targets[i] ? Narrow(ModulePath(targets[i])).c_str() : "-");
        if (!targets[i]) return false;
    }
    api = {reinterpret_cast<NvInitialize>(targets[0]), reinterpret_cast<NvCreate>(targets[1]),
        reinterpret_cast<NvLaunch>(targets[2]), reinterpret_cast<NvDestroy>(targets[3]),
        reinterpret_cast<NvDescriptorObject>(targets[4])};
    const int initialized = api.initialize();
    Say("SETUP NvAPI_Initialize=%d", initialized);
    return initialized == 0;
}

bool PickAdapter(ComPtr<IDXGIAdapter1>& chosen)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { Say("SETUP CreateDXGIFactory1 failed"); return false; }
    Say("SETUP dxgi.dll=%s", Narrow(ModulePath(*reinterpret_cast<void**>(factory.Get()))).c_str());
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        Say("SETUP adapter %u vendor=0x%04x device=0x%04x name=%s", i, desc.VendorId, desc.DeviceId, Narrow(desc.Description).c_str());
        if (desc.VendorId == 0x10DE && !chosen) chosen = adapter;
    }
    return chosen != nullptr;
}

bool CreateGpu(IDXGIAdapter1* adapter, Gpu& gpu)
{
    HRESULT created = E_FAIL;
    for (D3D_FEATURE_LEVEL level : {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0})
    {
        created = D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&gpu.device));
        if (SUCCEEDED(created)) { Say("SETUP device feature level 0x%x", level); break; }
    }
    if (FAILED(created)) { Say("SETUP D3D12CreateDevice failed 0x%08lx", created); return false; }
    const void* table = *reinterpret_cast<void**>(gpu.device.Get());
    Say("SETUP device vtable module=%s", Narrow(ModulePath(*reinterpret_cast<void* const*>(table))).c_str());
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue));
    const auto* deviceTable = *reinterpret_cast<void* const* const*>(gpu.device.Get());
    for (size_t slot : {10, 11, 14, 16, 18, 19})
        Say("SETUP device vtable slot %zu module=%s", slot, Narrow(ModulePath(deviceTable[slot])).c_str());
    if (gpu.queue)
    {
        const auto* queueTable = *reinterpret_cast<void* const* const*>(gpu.queue.Get());
        Say("SETUP queue vtable slot 10 (ExecuteCommandLists) module=%s", Narrow(ModulePath(queueTable[10])).c_str());
    }
    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return gpu.queue && gpu.allocator && gpu.fence;
}

struct Probe
{
    Gpu gpu;
    Nvapi api;
    std::array<void*, 3> kernels{};
    ComPtr<ID3D12Resource> input, output, output2, texture;
    ComPtr<ID3D12DescriptorHeap> visible, hidden, big;
    ComPtr<ID3D12Device> native;
    UINT increment{}, nativeIncrement{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kLayers]{};
    UINT64 textureBytes{};
    uint64_t textureHandle{}, surfaceHandle{}, typedHandle{};

    static D3D12_CPU_DESCRIPTOR_HANDLE Slot(ID3D12DescriptorHeap* heap, UINT slot, UINT stride)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot) * stride;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE VisibleSlot(UINT slot) const { return Slot(visible.Get(), slot, increment); }

    const char* DeviceKind(ID3D12Device* device) const
    {
        if (gpu.device.Get() == native.Get()) return "same";
        return device == native.Get() ? "native" : "wrapped";
    }

    bool ResolveNative()
    {
        const bool unwrapped = single_overlay::native::Unwrap(gpu.device.Get(), native) && native;
        if (!unwrapped) native = gpu.device;
        nativeIncrement = native->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Say("SETUP native device: Unwrap %s, wrapped=%p native=%p wrapped_module=%s native_module=%s", unwrapped ? "ok" : "FAILED",
            static_cast<void*>(gpu.device.Get()), static_cast<void*>(native.Get()), ObjectModule(gpu.device.Get()).c_str(),
            ObjectModule(native.Get()).c_str());
        Say("SETUP descriptor increment wrapped=%u native=%u", increment, nativeIncrement);
        return unwrapped;
    }

    void DescribeRow(const char* id, const char* what, ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE descriptor,
        ID3D12Device* device, int type, const char* detail, uint64_t* keep = nullptr)
    {
        DescriptorObject request{sizeof(DescriptorObject), 0, device, type, descriptor, 0};
        const int code = GuardedObject(api.object, &request);
        const uint64_t handle = request.handle;
        Result(id, what, code, handle, code == 0 && handle, detail);
        if (keep) *keep = (code == 0) ? handle : 0;
        Say("DESC   %-6s code=%-4d heap_cpu_start=0x%llx unwrapped_cpu_start=%s descriptor=0x%llx device=%s %p module=%s",
            id, code, static_cast<unsigned long long>(heap->GetCPUDescriptorHandleForHeapStart().ptr), UnwrappedStart(heap).c_str(),
            static_cast<unsigned long long>(descriptor.ptr), DeviceKind(device), static_cast<void*>(device),
            ObjectModule(device).c_str());
    }

    void CreateResources()
    {
        constexpr auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        input = gpu.Buffer(D3D12_HEAP_TYPE_DEFAULT, kInSize, uav, D3D12_RESOURCE_STATE_COPY_DEST);
        output = gpu.Buffer(D3D12_HEAP_TYPE_DEFAULT, kOutSize, uav, D3D12_RESOURCE_STATE_COPY_DEST);
        output2 = gpu.Buffer(D3D12_HEAP_TYPE_DEFAULT, kOutSize, uav, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = kLayers;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = uav;
        gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&texture));
        UINT rows[kLayers]{};
        UINT64 rowBytes[kLayers]{};
        gpu.device->GetCopyableFootprints(&desc, 0, kLayers, kUploadTexture, footprints, rows, rowBytes, &textureBytes);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 8;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&visible));
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&hidden));
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap.NumDescriptors = kBigDescriptors;
        gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&big));
        increment = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC TextureSrv() const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = kLayers;
        return srv;
    }

    static D3D12_UNORDERED_ACCESS_VIEW_DESC TextureUav()
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.ArraySize = kLayers;
        return uav;
    }

    void CreateViews()
    {
        const D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv = TextureSrv();
        const D3D12_UNORDERED_ACCESS_VIEW_DESC uav = TextureUav();
        gpu.device->CreateShaderResourceView(texture.Get(), &textureSrv, VisibleSlot(0));
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, VisibleSlot(1));
        D3D12_SHADER_RESOURCE_VIEW_DESC typed{};
        typed.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        typed.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        typed.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        typed.Buffer.FirstElement = kFirstElement;
        typed.Buffer.NumElements = kElements;
        gpu.device->CreateShaderResourceView(input.Get(), &typed, VisibleSlot(2));
        gpu.device->CreateShaderResourceView(texture.Get(), &textureSrv, hidden->GetCPUDescriptorHandleForHeapStart());
        gpu.device->CreateShaderResourceView(texture.Get(), &textureSrv, Slot(big.Get(), kRouteSrvSlot, increment));
        gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, Slot(big.Get(), kRouteUavSlot, increment));
    }

    bool Upload()
    {
        ComPtr<ID3D12Resource> upload = gpu.Buffer(D3D12_HEAP_TYPE_UPLOAD, kUploadTexture + textureBytes, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* mapped{};
        if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        std::vector<Uint4> inputData(kInSize / 16, Uint4{0x5A5A5A5Au, 0x5A5A5A5Au, 0x5A5A5A5Au, 0x5A5A5A5Au});
        for (uint32_t g = kFirstElement; g < kFirstElement + kElements; ++g) inputData[g] = InputElement(g);
        memcpy(mapped + kUploadIn, inputData.data(), kInSize);
        std::vector<uint32_t> sentinel(kOutSize / 4, kSentinel);
        memcpy(mapped + kUploadOut, sentinel.data(), kOutSize);
        memcpy(mapped + kUploadOut2, sentinel.data(), kOutSize);
        for (uint32_t l = 0; l < kLayers; ++l)
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    const Uint4 texel = InputTexel((l * kHeight + y) * kWidth + x);
                    memcpy(mapped + footprints[l].Offset + y * footprints[l].Footprint.RowPitch + x * 16, &texel, 16);
                }
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12GraphicsCommandList> list = gpu.NewList();
        list->CopyBufferRegion(input.Get(), 0, upload.Get(), kUploadIn, kInSize);
        list->CopyBufferRegion(output.Get(), 0, upload.Get(), kUploadOut, kOutSize);
        list->CopyBufferRegion(output2.Get(), 0, upload.Get(), kUploadOut2, kOutSize);
        for (uint32_t l = 0; l < kLayers; ++l)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = l;
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[l];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(output2.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(4, barriers);
        return gpu.Submit(list.Get());
    }

    void DescribeAll()
    {
        ID3D12Device* wrapped = gpu.device.Get();
        ID3D12DescriptorHeap* heap = visible.Get();
        DescribeRow("a", "Texture2DArray SRV, texture, shader-visible", heap, VisibleSlot(0), wrapped, kTextureObject, "", &textureHandle);
        DescribeRow("b", "RWTexture2DArray UAV, surface, shader-visible", heap, VisibleSlot(1), wrapped, kSurfaceObject, "", &surfaceHandle);
        DescribeRow("c", "typed Buffer SRV (weights case), texture", heap, VisibleSlot(2), wrapped, kTextureObject, "", &typedHandle);
        DescribeRow("a-nv", "Texture2DArray SRV, texture, non-visible heap", hidden.Get(), hidden->GetCPUDescriptorHandleForHeapStart(),
            wrapped, kTextureObject, "(a with a non-shader-visible CPU handle)");
        gpu.device->CopyDescriptorsSimple(1, VisibleSlot(3), hidden->GetCPUDescriptorHandleForHeapStart(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        DescribeRow("a-cp", "SRV copied from a non-visible heap, texture", heap, VisibleSlot(3), wrapped, kTextureObject,
            "(CopyDescriptorsSimple into the visible heap, then a)");
        const D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv = TextureSrv();
        const D3D12_CPU_DESCRIPTOR_HANDLE last = Slot(big.Get(), kBigDescriptors - 1, increment);
        gpu.device->CreateShaderResourceView(texture.Get(), &textureSrv, last);
        DescribeRow("a-big", "SRV in the last slot of a 100000-slot heap", big.Get(), last, wrapped, kTextureObject, "");
    }

    void DescribeMismatches()
    {
        DescribeRow("a-x", "Texture2DArray SRV asked as surface (type 0)", visible.Get(), VisibleSlot(0), native.Get(), kSurfaceObject,
            "(type mismatch)");
        DescribeRow("b-x", "RWTexture2DArray UAV asked as texture (type 1)", visible.Get(), VisibleSlot(1), native.Get(), kTextureObject,
            "(type mismatch)");
        DescribeRow("empty", "never-written slot 40 of the big heap, texture", big.Get(), Slot(big.Get(), kEmptySlot, nativeIncrement),
            native.Get(), kTextureObject, "(no descriptor written)");
    }

    void DescribeRoute(const char* id, int type, ID3D12DescriptorHeap* heap, UINT slot, const char* what)
    {
        ID3D12Device* wrapped = gpu.device.Get();
        const std::string base = id;
        const std::string title = what;
        const D3D12_CPU_DESCRIPTOR_HANDLE own = Slot(heap, slot, nativeIncrement);
        DescribeRow((base + "-rn").c_str(), (title + ", native device, heap object start").c_str(), heap, own, native.Get(), type,
            "(the route as written)");
        DescribeRow((base + "-rw").c_str(), (title + ", wrapped device, heap object start").c_str(), heap, own, wrapped, type, "");
        const ComPtr<ID3D12DescriptorHeap> unwrapped = UnwrapHeap(heap);
        if (!unwrapped)
        {
            Say("RESULT %-4s %s, native device, unwrapped heap start: heap cannot be unwrapped", (base + "-ru").c_str(), what);
            return;
        }
        DescribeRow((base + "-ru").c_str(), (title + ", native device, unwrapped heap start").c_str(), unwrapped.Get(),
            Slot(unwrapped.Get(), slot, nativeIncrement), native.Get(), type, "");
    }

    void DescribeRoutes()
    {
        DescribeRoute("a", kTextureObject, visible.Get(), 0, "route Texture2DArray SRV slot 0");
        DescribeRoute("b", kSurfaceObject, visible.Get(), 1, "route RWTexture2DArray UAV slot 1");
        DescribeRoute("a32", kTextureObject, big.Get(), kRouteSrvSlot, "route Texture2DArray SRV slot 32 of the big heap");
        DescribeRoute("b33", kSurfaceObject, big.Get(), kRouteUavSlot, "route RWTexture2DArray UAV slot 33 of the big heap");
    }

    bool Launch(const char* id, const char* what, int kernel, uint32_t groups, const void* params, uint32_t size)
    {
        ComPtr<ID3D12GraphicsCommandList> list = gpu.NewList();
        ID3D12DescriptorHeap* heaps[] = {visible.Get()};
        list->SetDescriptorHeaps(1, heaps);
        const int launched = GuardedLaunch(api.launch, list.Get(), kernels[kernel], groups, 1, 1, params, size);
        if (launched != 0)
        {
            list->Close();
            Result(id, what, launched, 0, false, "LaunchCubinShader refused");
            return false;
        }
        const D3D12_RESOURCE_BARRIER barrier = UavBarrier();
        list->ResourceBarrier(1, &barrier);
        const bool done = gpu.Submit(list.Get());
        const HRESULT removed = gpu.device->GetDeviceRemovedReason();
        if (!done || FAILED(removed))
        {
            char detail[64];
            snprintf(detail, sizeof(detail), "fence %s, removed reason 0x%08lx", done ? "signalled" : "timed out", removed);
            Result(id, what, 0, 0, false, detail);
            return false;
        }
        return true;
    }

    bool ReadBack(std::vector<uint8_t>& bytes)
    {
        ComPtr<ID3D12Resource> readback = gpu.Buffer(D3D12_HEAP_TYPE_READBACK, kReadbackTexture + textureBytes,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        ComPtr<ID3D12GraphicsCommandList> list = gpu.NewList();
        const D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(output2.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(3, barriers);
        list->CopyBufferRegion(readback.Get(), kReadbackOut, output.Get(), 0, kOutSize);
        list->CopyBufferRegion(readback.Get(), kReadbackOut2, output2.Get(), 0, kOutSize);
        for (uint32_t l = 0; l < kLayers; ++l)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprints[l];
            destination.PlacedFootprint.Offset = footprints[l].Offset - kUploadTexture + kReadbackTexture;
            source.pResource = texture.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = l;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        if (!gpu.Submit(list.Get())) return false;
        uint8_t* mapped{};
        if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        bytes.assign(mapped, mapped + kReadbackTexture + textureBytes);
        readback->Unmap(0, nullptr);
        return true;
    }

    static Mismatches CheckBuffer(const uint8_t* base, uint32_t dataOffset)
    {
        Mismatches found;
        for (uint32_t i = 0; i < kOutSize / 16; ++i)
        {
            Uint4 got{};
            memcpy(&got, base + i * 16, 16);
            const bool inside = i * 16 >= dataOffset && i * 16 < dataOffset + kBytes;
            const Uint4 expected = inside ? Transform(InputElement(kFirstElement + (i * 16 - dataOffset) / 16))
                                          : Uint4{kSentinel, kSentinel, kSentinel, kSentinel};
            if (!Equal(got, expected)) found.Note("element " + std::to_string(i), got, expected);
        }
        return found;
    }

    Mismatches CheckTexture(const uint8_t* base) const
    {
        Mismatches found;
        for (uint32_t l = 0; l < kLayers; ++l)
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    Uint4 got{};
                    memcpy(&got, base + footprints[l].Offset - kUploadTexture + kReadbackTexture + y * footprints[l].Footprint.RowPitch + x * 16, 16);
                    const Uint4 expected = TransformImage(InputTexel((l * kHeight + y) * kWidth + x));
                    if (!Equal(got, expected))
                        found.Note("(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(l) + ")", got, expected);
                }
        return found;
    }

    bool CreateKernels()
    {
        std::vector<uint8_t> cubin;
        if (!ReadFile(ExeDirectory() + L"probe_kernels.cubin", cubin)) { Say("SETUP probe_kernels.cubin missing"); return false; }
        const char* names[] = {"probe_raw", "probe_image", "probe_typed"};
        bool all = true;
        for (size_t i = 0; i < kernels.size(); ++i)
        {
            const int code = GuardedCreate(api.create, gpu.device.Get(), cubin.data(), uint32_t(cubin.size()), names[i], &kernels[i]);
            Say("SETUP CreateCubinComputeShaderWithName %s code=%d handle=%p", names[i], code, kernels[i]);
            if (code != 0 || !kernels[i]) { kernels[i] = nullptr; all = false; }
        }
        return all;
    }

    void RunKernels()
    {
        const uint64_t inputAddress = input->GetGPUVirtualAddress(), outputAddress = output->GetGPUVirtualAddress(),
            output2Address = output2->GetGPUVirtualAddress();
        Say("SETUP gpu va input=0x%llx output=0x%llx output2=0x%llx", static_cast<unsigned long long>(inputAddress),
            static_cast<unsigned long long>(outputAddress), static_cast<unsigned long long>(output2Address));
        bool ranRaw = false, ranImage = false, ranTyped = false;
        if (kernels[0])
        {
            const RawParams params{inputAddress + kInOffset, outputAddress + kOutOffset, kElements, 0};
            ranRaw = Launch("d", "raw GPU VA read + write (+ offsets)", 0, kElements / 128, &params, sizeof(params));
        }
        else Skipped("d", "raw GPU VA read + write (+ offsets)", "kernel probe_raw not created");
        if (kernels[1] && textureHandle && surfaceHandle)
        {
            const ImageParams params{textureHandle, surfaceHandle, kWidth, kHeight, kLayers, 0};
            ranImage = Launch("e", "texture object read + surface store (in place)", 1, kWidth * kHeight * kLayers / 128, &params, sizeof(params));
        }
        else Skipped("e", "texture object read + surface store (in place)", "needs kernel and handles of (a) and (b)");
        if (kernels[2] && typedHandle)
        {
            const TypedParams params{typedHandle, output2Address + kOutOffset, kElements, 0};
            ranTyped = Launch("f", "typed buffer texture object read (tex.1d)", 2, kElements / 128, &params, sizeof(params));
        }
        else Skipped("f", "typed buffer texture object read (tex.1d)", "needs kernel and handle of (c)");
        std::vector<uint8_t> bytes;
        if (!ReadBack(bytes)) { Say("SETUP readback failed"); gAllPassed = false; return; }
        if (ranRaw)
            Report("d", "raw GPU VA read + write (+ offsets)", outputAddress + kOutOffset,
                CheckBuffer(bytes.data() + kReadbackOut, kOutOffset), "256 uint4 exact, sentinel intact");
        if (ranImage)
            Report("e", "texture object read + surface store (in place)", surfaceHandle,
                CheckTexture(bytes.data()), "256 texels exact");
        if (ranTyped)
            Report("f", "typed buffer texture object read (tex.1d)", typedHandle,
                CheckBuffer(bytes.data() + kReadbackOut2, kOutOffset), "256 uint4 exact, sentinel intact");
    }
};
}

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter(OnCrash);
    gOut = fopen(argc > 1 ? argv[1] : "probe_results.txt", "w");
    Say("SETUP exe=%s", Narrow(ModulePath(reinterpret_cast<void*>(&main))).c_str());
    Say("SETUP wine=%s", GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") ? "yes" : "no");
    Say("SETUP reshade=%s", GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "ReShadeRegisterAddon") ? "yes" : "no");
    for (const wchar_t* name : {L"d3d12.dll", L"dxgi.dll"})
    {
        const HMODULE module = GetModuleHandleW(name);
        wchar_t path[MAX_PATH]{};
        if (module) GetModuleFileNameW(module, path, MAX_PATH);
        Say("SETUP loaded %s=%s", Narrow(name).c_str(), module ? Narrow(path).c_str() : "(not loaded yet)");
    }
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const void* createDevice = d3d12 ? reinterpret_cast<const void*>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    Say("SETUP D3D12CreateDevice export resolves in %s", createDevice ? Narrow(ModulePath(createDevice)).c_str() : "(missing)");
    Probe probe;
    if (!LoadNvapi(probe.api)) { Say("SUMMARY nvapi unavailable"); return 2; }
    ComPtr<IDXGIAdapter1> adapter;
    if (!PickAdapter(adapter)) { Say("SUMMARY no NVIDIA adapter"); return 2; }
    if (!CreateGpu(adapter.Get(), probe.gpu)) { Say("SUMMARY device creation failed"); return 2; }
    probe.CreateResources();
    probe.ResolveNative();
    probe.CreateViews();
    if (!probe.Upload()) { Say("SUMMARY upload failed"); return 2; }
    probe.CreateKernels();
    probe.DescribeAll();
    probe.DescribeRoutes();
    probe.DescribeMismatches();
    probe.RunKernels();
    Say("SUMMARY %s", gAllPassed ? "all pass" : "some fail or skipped");
    if (gOut) fclose(gOut);
    return gAllPassed ? 0 : 1;
}
