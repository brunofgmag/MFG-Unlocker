#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "optiscaler_host.h"
#include "single_module.h"
#include "xess/cuda/cuda_route.h"
#include "xess/cuda/provider_list.h"
#include "xess/runtime_layout.h"
#include "xess/runtime_payload.h"
#include "xess/xess_mod.h"
#include "xmx/xmx_context.h"
#include "xmx/xmx_hud_composite.h"
#include "xmx/xmx_rules.h"
#include "xmx/xmx_tensor_cores.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
HMODULE gPayload = nullptr;

constexpr UINT kPairsPerList = 10;
constexpr UINT kQueryCapacity = 32768;

enum class Content { Zero, Scene, Noise };

const char* NameOf(Content content)
{
    switch (content)
    {
    case Content::Scene: return "scene";
    case Content::Noise: return "noise";
    default: return "zero";
    }
}

bool ParseContent(const std::wstring& text, Content& content)
{
    for (const auto candidate : {Content::Zero, Content::Scene, Content::Noise})
        if (text == fs::path(NameOf(candidate)).wstring())
        {
            content = candidate;
            return true;
        }
    return false;
}

struct Options
{
    UINT outputWidth = 3840, outputHeight = 2160;
    UINT renderWidth = 2560, renderHeight = 1440;
    UINT frames = 3, pairs = 300, warmup = 30;
    bool dxil = false, declined = false, hud = true, expectWrapper = false;
    Content content = Content::Zero;
    std::wstring folder, payload, programs;
};

bool ParseSize(const wchar_t* text, UINT& width, UINT& height)
{
    wchar_t* end = nullptr;
    const unsigned long first = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'x') return false;
    const wchar_t* rest = end + 1;
    const unsigned long second = std::wcstoul(rest, &end, 10);
    if (end == rest || *end != L'\0' || first == 0 || second == 0) return false;
    width = static_cast<UINT>(first);
    height = static_cast<UINT>(second);
    return true;
}

bool ParseCount(const wchar_t* text, UINT& count)
{
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0') return false;
    count = static_cast<UINT>(value);
    return true;
}

bool ParseOptions(int argc, wchar_t** argv, Options& options)
{
    if (argc < 3) return false;
    options.folder = argv[1];
    options.payload = argv[2];
    for (int i = 3; i < argc; ++i)
    {
        const std::wstring name = argv[i];
        if (name == L"--dxil") options.dxil = true;
        else if (name == L"--declined") options.declined = true;
        else if (name == L"--no-hud") options.hud = false;
        else if (name == L"--expect-wrapper") options.expectWrapper = true;
        else if (i + 1 >= argc) return false;
        else if (name == L"--programs") options.programs = argv[++i];
        else if (name == L"--content") { if (!ParseContent(argv[++i], options.content)) return false; }
        else if (name == L"--output") { if (!ParseSize(argv[++i], options.outputWidth, options.outputHeight)) return false; }
        else if (name == L"--render") { if (!ParseSize(argv[++i], options.renderWidth, options.renderHeight)) return false; }
        else if (name == L"--frames") { if (!ParseCount(argv[++i], options.frames)) return false; }
        else if (name == L"--pairs") { if (!ParseCount(argv[++i], options.pairs)) return false; }
        else if (name == L"--warmup") { if (!ParseCount(argv[++i], options.warmup)) return false; }
        else return false;
    }
    return options.frames > 0 && options.pairs > 0 && !(options.dxil && options.declined);
}

ComPtr<ID3D12Device> HardwareDevice(std::wstring& adapterName)
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
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) continue;
        adapterName = desc.Description;
        return device;
    }
}

void** TableOf(void* object)
{
    return *static_cast<void***>(object);
}

std::wstring ImageName(const void* address)
{
    HMODULE owner{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &owner) || !GetModuleFileNameW(owner, path, MAX_PATH)) return {};
    return fs::path(path).filename().wstring();
}

bool DeviceIsAcceptable(const Options& options, ID3D12Device* device)
{
    const std::wstring image = ImageName(TableOf(device));
    const bool wrapped = _wcsicmp(image.c_str(), L"D3D12Core.dll") && _wcsicmp(image.c_str(), L"d3d12.dll");
    std::printf("device vtable in %ls (%s)\n", image.c_str(), wrapped ? "a wrapper" : "the runtime");
    if (options.expectWrapper && !wrapped)
    {
        std::printf("FAIL --expect-wrapper: the device vtable is in %ls, the runtime, so ReShade did not wrap it\n",
            image.c_str());
        return false;
    }
    return true;
}

struct Program
{
    std::string sha;
    size_t bytes;
};

struct ProgramBook
{
    std::mutex lock;
    fs::path folder;
    bool recording = false;
    std::map<const void*, Program> byPipeline;
    std::set<std::string> dumped;
};

ProgramBook gBook;

using CreateComputeFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStreamFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
CreateComputeFn gCreateCompute{};
CreateStreamFn gCreateStream{};

struct SubobjectShape
{
    size_t alignment, size;
};

SubobjectShape ShapeOf(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
    switch (type)
    {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return {alignof(void*), sizeof(void*)};
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return {alignof(D3D12_SHADER_BYTECODE), sizeof(D3D12_SHADER_BYTECODE)};
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return {alignof(UINT), sizeof(UINT)};
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        return {alignof(D3D12_CACHED_PIPELINE_STATE), sizeof(D3D12_CACHED_PIPELINE_STATE)};
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return {alignof(UINT), sizeof(UINT)};
    default: return {1, 0};
    }
}

size_t AlignedTo(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

D3D12_SHADER_BYTECODE StreamShader(const D3D12_PIPELINE_STATE_STREAM_DESC& stream)
{
    const auto* base = static_cast<const std::byte*>(stream.pPipelineStateSubobjectStream);
    size_t offset = 0;
    while (offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= stream.SizeInBytes)
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, base + offset, sizeof(type));
        const auto shape = ShapeOf(type);
        const size_t data = AlignedTo(sizeof(type), shape.alignment);
        if (!shape.size || offset + data + shape.size > stream.SizeInBytes) return {};
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
        {
            D3D12_SHADER_BYTECODE code;
            std::memcpy(&code, base + offset + data, sizeof(code));
            return code;
        }
        offset += AlignedTo(data + shape.size, sizeof(void*));
    }
    return {};
}

void WriteProgram(const std::string& sha, std::span<const unsigned char> bytes)
{
    std::ofstream file(gBook.folder / (sha + ".dxil"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void Record(const void* pipeline, const D3D12_SHADER_BYTECODE& code)
{
    try
    {
        Program program{"none", 0};
        const std::span<const unsigned char> bytes(static_cast<const unsigned char*>(code.pShaderBytecode),
            code.pShaderBytecode ? code.BytecodeLength : 0);
        const bool hasCode = !bytes.empty();
        if (hasCode) program = {xess_payload::Hash(bytes), bytes.size()};
        std::lock_guard guard(gBook.lock);
        if (hasCode && gBook.dumped.insert(program.sha).second) WriteProgram(program.sha, bytes);
        gBook.byPipeline[pipeline] = program;
    }
    catch (...)
    {
    }
}

HRESULT STDMETHODCALLTYPE RecordCompute(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid,
    void** out)
{
    const HRESULT result = gCreateCompute(self, desc, iid, out);
    if (SUCCEEDED(result) && desc && out && *out) Record(*out, desc->CS);
    return result;
}

HRESULT STDMETHODCALLTYPE RecordStream(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid,
    void** out)
{
    const HRESULT result = gCreateStream(self, desc, iid, out);
    if (SUCCEEDED(result) && desc && out && *out) Record(*out, StreamShader(*desc));
    return result;
}

bool Patch(void** slot, void* hook)
{
    DWORD old{}, unused{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &unused);
    return true;
}

bool RecordPrograms(ID3D12Device* device, const std::wstring& folder)
{
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) return false;
    gBook.folder = folder;
    void** table = TableOf(device);
    gCreateCompute = reinterpret_cast<CreateComputeFn>(table[11]);
    gCreateStream = reinterpret_cast<CreateStreamFn>(table[47]);
    gBook.recording = Patch(&table[11], reinterpret_cast<void*>(&RecordCompute)) &&
        Patch(&table[47], reinterpret_cast<void*>(&RecordStream));
    return gBook.recording;
}

std::optional<Program> ProgramOf(const void* pipeline)
{
    std::lock_guard guard(gBook.lock);
    const auto found = gBook.byPipeline.find(pipeline);
    if (found == gBook.byPipeline.end()) return std::nullopt;
    return found->second;
}

std::string ShaColumn(const void* pipeline)
{
    if (!gBook.recording) return {};
    const auto program = ProgramOf(pipeline);
    return "  " + (program ? program->sha.substr(0, 12) : std::string("?"));
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
    WaitForSingleObject(gpu.event, 60000);
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

struct Timer
{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    UINT used = 0;
    bool overflow = false;

    UINT Stamp(ID3D12GraphicsCommandList* list) noexcept
    {
        if (used >= kQueryCapacity)
        {
            overflow = true;
            return 0;
        }
        list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, used);
        return used++;
    }
};

bool MakeTimer(ID3D12Device* device, Timer& timer)
{
    D3D12_QUERY_HEAP_DESC heap{};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = kQueryCapacity;
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(UINT64) * kQueryCapacity;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&timer.heap)))
        && SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timer.readback)));
}

struct Scene
{
    ComPtr<ID3D12Resource> color[2], hudless[2], depth[2], motion[2];
    std::vector<ComPtr<ID3D12Resource>> outputs;
};

bool MakeScene(ID3D12Device* device, const Options& options, Scene& scene)
{
    const auto input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool ok = true;
    for (int i = 0; i < 2; ++i)
    {
        scene.color[i] = MakeTexture(device, options.outputWidth, options.outputHeight,
            DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_NONE, input);
        scene.hudless[i] = MakeTexture(device, options.outputWidth, options.outputHeight,
            DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_NONE, input);
        scene.depth[i] = MakeTexture(device, options.renderWidth, options.renderHeight, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, input);
        scene.motion[i] = MakeTexture(device, options.renderWidth, options.renderHeight, DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, input);
        ok = ok && scene.color[i] && scene.hudless[i] && scene.depth[i] && scene.motion[i];
    }
    scene.outputs.resize(options.frames);
    for (auto& output : scene.outputs)
    {
        output = MakeTexture(device, options.outputWidth, options.outputHeight, DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ok = ok && output;
    }
    return ok;
}

struct Rect
{
    float x, y, w, h, vx, vy, depth;
};

struct Planes
{
    std::vector<uint32_t> color, motion;
    std::vector<float> depth;
};

struct Texel
{
    bool read = false;
    UINT x = 0, y = 0;
    float u = 0, v = 0;
};

constexpr float kPanX = -24.f, kPanY = 6.f;
constexpr int kObjects = 8;

uint32_t Next(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float Between(uint32_t& state, float low, float high)
{
    return low + (high - low) * (static_cast<float>(Next(state) >> 8) / 16777216.f);
}

uint16_t HalfOf(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
    if (exponent <= 0) return sign;
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7BFF);
    const uint32_t rounded = (static_cast<uint32_t>(exponent) << 10) + ((bits >> 13) & 0x3FF) + ((bits >> 12) & 1);
    return static_cast<uint16_t>(sign | rounded);
}

float FloatOf(uint16_t half)
{
    const int exponent = (half >> 10) & 0x1F;
    const float magnitude = std::ldexp(static_cast<float>((half & 0x3FF) | (exponent ? 0x400 : 0)), (exponent ? exponent : 1) - 25);
    return (half & 0x8000) ? -magnitude : magnitude;
}

uint32_t PackedMotion(float x, float y)
{
    return HalfOf(x) | (static_cast<uint32_t>(HalfOf(y)) << 16);
}

uint32_t PackedColor(float r, float g, float b)
{
    const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.f, 1.f) * 1023.f + 0.5f); };
    return channel(r) | (channel(g) << 10) | (channel(b) << 20) | (3u << 30);
}

std::vector<Rect> MakeObjects(const Options& options)
{
    uint32_t state = 0x2545F491u;
    const float width = static_cast<float>(options.renderWidth), height = static_cast<float>(options.renderHeight);
    std::vector<Rect> objects;
    for (int i = 0; i < kObjects; ++i)
    {
        Rect rect{};
        rect.w = std::floor(Between(state, 120.f, 400.f));
        rect.h = std::floor(Between(state, 80.f, 240.f));
        rect.x = std::floor(Between(state, 0.f, std::max(1.f, width - rect.w)));
        rect.y = std::floor(Between(state, 0.f, std::max(1.f, height - rect.h)));
        const float angle = Between(state, 0.f, 6.2831853f), speed = Between(state, 30.f, 80.f);
        rect.vx = std::cos(angle) * speed;
        rect.vy = std::sin(angle) * speed;
        rect.depth = Between(state, 0.6f, 0.9f);
        objects.push_back(rect);
    }
    return objects;
}

std::vector<Rect> PlacedFor(std::vector<Rect> objects, int slot)
{
    for (auto& rect : objects)
    {
        rect.x += rect.vx * static_cast<float>(slot);
        rect.y += rect.vy * static_cast<float>(slot);
    }
    return objects;
}

int Topmost(const std::vector<Rect>& objects, float x, float y)
{
    int found = -1;
    for (size_t i = 0; i < objects.size(); ++i)
    {
        const auto& rect = objects[i];
        const bool inside = x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
        if (inside && (found < 0 || rect.depth > objects[found].depth)) found = static_cast<int>(i);
    }
    return found;
}

uint32_t Pattern(int x, int y, int cell, float tint)
{
    const bool bright = (((x >> cell) + (y >> cell)) & 1) != 0;
    const float gradient = 0.5f + 0.5f * std::sin(static_cast<float>(x) * 0.0021f + static_cast<float>(y) * 0.0013f);
    const float level = (bright ? 0.85f : 0.3f) * (0.5f + 0.5f * gradient);
    const bool line = ((x % 37) + 37) % 37 == 0 || ((y % 37) + 37) % 37 == 0;
    if (line) return PackedColor(1.f, 1.f, 1.f);
    return PackedColor(level * gradient, level * (1.f - gradient), level * tint);
}

uint32_t SceneColor(const std::vector<Rect>& objects, const Options& options, int slot, UINT x, UINT y)
{
    const float scaleX = static_cast<float>(options.outputWidth) / static_cast<float>(options.renderWidth);
    const float scaleY = static_cast<float>(options.outputHeight) / static_cast<float>(options.renderHeight);
    const int top = Topmost(objects, (static_cast<float>(x) + 0.5f) / scaleX, (static_cast<float>(y) + 0.5f) / scaleY);
    if (top >= 0)
        return Pattern(static_cast<int>(x - objects[top].x * scaleX), static_cast<int>(y - objects[top].y * scaleY), 4,
            0.3f + 0.1f * static_cast<float>(top));
    return Pattern(static_cast<int>(x) - static_cast<int>(kPanX * static_cast<float>(slot) * scaleX),
        static_cast<int>(y) - static_cast<int>(kPanY * static_cast<float>(slot) * scaleY), 6, 0.6f);
}

void FillSceneColor(Planes& planes, const Options& options, const std::vector<Rect>& objects, int slot)
{
    planes.color.resize(static_cast<size_t>(options.outputWidth) * options.outputHeight);
    for (UINT y = 0; y < options.outputHeight; ++y)
        for (UINT x = 0; x < options.outputWidth; ++x)
            planes.color[static_cast<size_t>(y) * options.outputWidth + x] = SceneColor(objects, options, slot, x, y);
}

void FillSceneRender(Planes& planes, const Options& options, const std::vector<Rect>& objects, int slot)
{
    const float width = static_cast<float>(options.renderWidth), height = static_cast<float>(options.renderHeight);
    const size_t count = static_cast<size_t>(options.renderWidth) * options.renderHeight;
    planes.depth.resize(count);
    planes.motion.resize(count);
    for (UINT y = 0; y < options.renderHeight; ++y)
        for (UINT x = 0; x < options.renderWidth; ++x)
        {
            const size_t at = static_cast<size_t>(y) * options.renderWidth + x;
            const int top = Topmost(objects, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            const float row = std::clamp((static_cast<float>(y) - kPanY * static_cast<float>(slot)) / height, 0.f, 1.f);
            planes.depth[at] = top >= 0 ? objects[top].depth : 0.02f + 0.28f * row;
            planes.motion[at] = top >= 0 ? PackedMotion(-objects[top].vx / width, -objects[top].vy / height)
                                         : PackedMotion(-kPanX / width, -kPanY / height);
        }
}

Planes ScenePlanes(const Options& options, int slot)
{
    const auto objects = PlacedFor(MakeObjects(options), slot);
    Planes planes;
    FillSceneColor(planes, options, objects, slot);
    FillSceneRender(planes, options, objects, slot);
    return planes;
}

Planes NoisePlanes(const Options& options, int slot)
{
    uint32_t state = 0x9E3779B9u * static_cast<uint32_t>(slot + 1);
    const float width = static_cast<float>(options.renderWidth), height = static_cast<float>(options.renderHeight);
    Planes planes;
    planes.color.resize(static_cast<size_t>(options.outputWidth) * options.outputHeight);
    for (auto& texel : planes.color) texel = Next(state) | 0xC0000000u;
    const size_t count = static_cast<size_t>(options.renderWidth) * options.renderHeight;
    planes.depth.resize(count);
    planes.motion.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        planes.depth[i] = Between(state, 0.f, 1.f);
        planes.motion[i] = PackedMotion(Between(state, -64.f, 64.f) / width, Between(state, -64.f, 64.f) / height);
    }
    return planes;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

UINT RowPitchOf(UINT width)
{
    return (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

D3D12_TEXTURE_COPY_LOCATION FootprintOf(ID3D12Resource* buffer, DXGI_FORMAT format, UINT width, UINT height)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint.Footprint = {format, width, height, 1, RowPitchOf(width)};
    return location;
}

bool Upload(Gpu& gpu, ID3D12Resource* texture, DXGI_FORMAT format, const void* pixels, UINT width, UINT height)
{
    const UINT pitch = RowPitchOf(width);
    auto buffer = MakeBuffer(gpu.device.Get(), D3D12_HEAP_TYPE_UPLOAD, static_cast<UINT64>(pitch) * height,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    if (!buffer || FAILED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    for (UINT row = 0; row < height; ++row)
        std::memcpy(mapped + static_cast<size_t>(row) * pitch, static_cast<const uint8_t*>(pixels) + static_cast<size_t>(row) * width * 4,
            static_cast<size_t>(width) * 4);
    buffer->Unmap(0, nullptr);
    const auto source = FootprintOf(buffer.Get(), format, width, height);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Transition(gpu.list.Get(), texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(gpu.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return Submit(gpu);
}

bool FillSlot(Gpu& gpu, const Options& options, Scene& scene, int slot)
{
    const Planes planes = options.content == Content::Scene ? ScenePlanes(options, slot) : NoisePlanes(options, slot);
    const auto color = DXGI_FORMAT_R10G10B10A2_UNORM;
    return Upload(gpu, scene.color[slot].Get(), color, planes.color.data(), options.outputWidth, options.outputHeight)
        && Upload(gpu, scene.hudless[slot].Get(), color, planes.color.data(), options.outputWidth, options.outputHeight)
        && Upload(gpu, scene.depth[slot].Get(), DXGI_FORMAT_R32_FLOAT, planes.depth.data(), options.renderWidth, options.renderHeight)
        && Upload(gpu, scene.motion[slot].Get(), DXGI_FORMAT_R16G16_FLOAT, planes.motion.data(), options.renderWidth, options.renderHeight);
}

bool ReadPacked(ID3D12Resource* buffer, uint32_t& packed)
{
    const D3D12_RANGE range{0, sizeof(packed)};
    void* mapped = nullptr;
    if (FAILED(buffer->Map(0, &range, &mapped))) return false;
    std::memcpy(&packed, mapped, sizeof(packed));
    const D3D12_RANGE none{0, 0};
    buffer->Unmap(0, &none);
    return true;
}

bool ReadMotionTexel(Gpu& gpu, ID3D12Resource* motion, UINT x, UINT y, Texel& texel)
{
    auto buffer = MakeBuffer(gpu.device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (!buffer) return false;
    const auto destination = FootprintOf(buffer.Get(), DXGI_FORMAT_R16G16_FLOAT, 1, 1);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = motion;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
    Transition(gpu.list.Get(), motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    Transition(gpu.list.Get(), motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    uint32_t packed = 0;
    if (!Submit(gpu) || !ReadPacked(buffer.Get(), packed)) return false;
    texel = {true, x, y, FloatOf(static_cast<uint16_t>(packed & 0xFFFF)), FloatOf(static_cast<uint16_t>(packed >> 16))};
    return true;
}

POINT ProbePoint(const Options& options)
{
    if (options.content != Content::Scene) return {0, 0};
    const auto objects = MakeObjects(options);
    return {static_cast<LONG>(objects.front().x + objects.front().w / 2),
        static_cast<LONG>(objects.front().y + objects.front().h / 2)};
}

bool FillScene(Gpu& gpu, const Options& options, Scene& scene, Texel& texel)
{
    if (options.content == Content::Zero) return true;
    for (int slot = 0; slot < 2; ++slot)
        if (!FillSlot(gpu, options, scene, slot)) return false;
    const POINT at = ProbePoint(options);
    return ReadMotionTexel(gpu, scene.motion[0].Get(), static_cast<UINT>(at.x), static_cast<UINT>(at.y), texel);
}

xmx::context::Input InputOf(ID3D12Resource* resource, UINT width, UINT height)
{
    return {resource, 0, 0, width, height};
}

xmx::context::InitParams InitOf(const Options& options)
{
    xmx::context::InitParams init{};
    init.width = options.outputWidth;
    init.height = options.outputHeight;
    init.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    init.flags = xmx::context::kFlagDepthInverted;
    init.maxFrames = options.frames;
    init.creationMask = 1;
    init.visibleMask = 1;
    return init;
}

xmx::context::FrameData FrameOf(const Scene& scene, const Options& options, int slot)
{
    xmx::context::FrameData data{};
    auto* color = options.hud ? scene.hudless[slot].Get() : scene.color[slot].Get();
    data.backbuffer = InputOf(color, options.outputWidth, options.outputHeight);
    data.depth = InputOf(scene.depth[slot].Get(), options.renderWidth, options.renderHeight);
    data.motion = InputOf(scene.motion[slot].Get(), options.renderWidth, options.renderHeight);
    if (options.hud) data.hudless = InputOf(scene.hudless[slot].Get(), options.outputWidth, options.outputHeight);
    for (int i = 0; i < 4; ++i) data.projection[i * 5] = 1.f;
    data.resolution[0] = static_cast<float>(options.outputWidth);
    data.resolution[1] = static_cast<float>(options.outputHeight);
    data.motionScale[0] = static_cast<float>(options.renderWidth);
    data.motionScale[1] = static_cast<float>(options.renderHeight);
    return data;
}

struct Stamped
{
    UINT query;
    xefg_cuda::DispatchSample sample;
};

struct Recorded
{
    UINT pair, frame, before, after, hud;
    std::vector<Stamped> dispatches;
};

struct Key
{
    ID3D12PipelineState* pipeline;
    uint8_t layer;
    bool launched;
    UINT x, y, z;
    bool operator==(const Key&) const = default;
};

struct Measured
{
    UINT pair;
    double total, hud, tail;
    std::vector<double> dispatchMs;
    std::vector<Key> keys;
};

using PassResult = std::vector<std::vector<Measured>>;

struct Bench
{
    Options options;
    Gpu gpu;
    HMODULE fg;
    const xmx::Rvas* rvas;
    void* handle = nullptr;
    Scene scene;
    Texel texel;
    Timer timer;
    xmx::hud_composite::Pass hud;
    double toMs = 0;
    std::vector<uint32_t> bases;
    std::vector<ID3D12Resource*> outputs;
};

struct ProbeTarget
{
    Timer* timer;
    std::vector<Stamped>* into;
};

void RecordDispatch(ID3D12GraphicsCommandList* recordedOn, const xefg_cuda::DispatchSample& sample,
    void* context) noexcept
{
    auto& target = *static_cast<ProbeTarget*>(context);
    try
    {
        target.into->push_back({target.timer->Stamp(recordedOn), sample});
    }
    catch (...)
    {
        target.timer->overflow = true;
    }
}

xmx::context::ExecuteParams ExecuteOf(const Bench& bench, int next, unsigned firstFrame)
{
    xmx::context::ExecuteParams execute{};
    execute.previous = FrameOf(bench.scene, bench.options, next ^ 1);
    execute.next = FrameOf(bench.scene, bench.options, next);
    execute.outputs = bench.outputs.data();
    execute.outputBases = bench.bases.data();
    execute.outputCount = static_cast<uint8_t>(bench.options.frames);
    execute.firstFrame = firstFrame;
    execute.frameCount = 1;
    return execute;
}

bool RecordExecute(Bench& bench, UINT pair, unsigned frame, bool probed, std::vector<Recorded>& recorded)
{
    const int next = pair & 1;
    auto* list = bench.gpu.list.Get();
    auto execute = ExecuteOf(bench, next, frame);
    Recorded record{pair, frame, 0, 0, 0, {}};
    ProbeTarget target{&bench.timer, &record.dispatches};
    const xefg_cuda::DispatchProbe probe{RecordDispatch, &target};
    if (probed) xefg_cuda::SetDispatchProbe(&probe);
    record.before = bench.timer.Stamp(list);
    const int result = xmx::tensor_cores::Execute(bench.fg, *bench.rvas, bench.handle, list, execute);
    record.after = bench.timer.Stamp(list);
    xefg_cuda::SetDispatchProbe(nullptr);
    if (result != 0)
    {
        std::printf("FAIL execute returned %d at pair %u, firstFrame %u\n", result, pair, frame);
        return false;
    }
    if (bench.options.hud
        && !bench.hud.Apply(list, bench.gpu.device.Get(), bench.outputs[frame], bench.scene.color[next].Get(),
            bench.scene.hudless[next].Get(), false))
    {
        std::printf("FAIL the HUD composite did not record: %s\n", bench.hud.Fault().c_str());
        return false;
    }
    record.hud = bench.timer.Stamp(list);
    recorded.push_back(std::move(record));
    return true;
}

double Delta(UINT64 later, UINT64 earlier)
{
    return static_cast<double>(static_cast<int64_t>(later - earlier));
}

Key KeyOf(const xefg_cuda::DispatchSample& sample)
{
    return {sample.pipeline, sample.layer, sample.launched, sample.x, sample.y, sample.z};
}

Measured Measure(const Recorded& record, const UINT64* stamps, double toMs)
{
    Measured measured{record.pair, Delta(stamps[record.after], stamps[record.before]) * toMs,
        Delta(stamps[record.hud], stamps[record.after]) * toMs, 0, {}, {}};
    UINT64 previous = stamps[record.before];
    for (const auto& dispatch : record.dispatches)
    {
        measured.dispatchMs.push_back(Delta(stamps[dispatch.query], previous) * toMs);
        measured.keys.push_back(KeyOf(dispatch.sample));
        previous = stamps[dispatch.query];
    }
    measured.tail = Delta(stamps[record.after], previous) * toMs;
    return measured;
}

bool CollectBatch(Bench& bench, const std::vector<Recorded>& recorded, PassResult& result)
{
    bench.gpu.list->ResolveQueryData(bench.timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, bench.timer.used,
        bench.timer.readback.Get(), 0);
    if (!Submit(bench.gpu))
    {
        std::printf("FAIL the batch did not complete on the GPU\n");
        return false;
    }
    const D3D12_RANGE range{0, sizeof(UINT64) * bench.timer.used};
    UINT64* stamps = nullptr;
    if (FAILED(bench.timer.readback->Map(0, &range, reinterpret_cast<void**>(&stamps))))
    {
        std::printf("FAIL the timestamp readback did not map\n");
        return false;
    }
    for (const auto& record : recorded)
        if (record.pair >= bench.options.warmup) result[record.frame].push_back(Measure(record, stamps, bench.toMs));
    const D3D12_RANGE none{0, 0};
    bench.timer.readback->Unmap(0, &none);
    return true;
}

bool RunPass(Bench& bench, bool probed, PassResult& result)
{
    result.assign(bench.options.frames, {});
    const UINT total = bench.options.warmup + bench.options.pairs;
    for (UINT first = 0; first < total; first += kPairsPerList)
    {
        std::vector<Recorded> recorded;
        bench.timer.used = 0;
        const UINT last = std::min(total, first + kPairsPerList);
        for (UINT pair = first; pair < last; ++pair)
            for (unsigned frame = 0; frame < bench.options.frames; ++frame)
                if (!RecordExecute(bench, pair, frame, probed, recorded)) return false;
        if (bench.timer.overflow)
        {
            std::printf("FAIL a batch needs more than %u timestamps\n", kQueryCapacity);
            return false;
        }
        if (!CollectBatch(bench, recorded, result)) return false;
    }
    return true;
}

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const size_t low = static_cast<size_t>(position);
    const size_t high = std::min(low + 1, values.size() - 1);
    return values[low] + (values[high] - values[low]) * (position - static_cast<double>(low));
}

struct Spread
{
    double median, p90;
};

Spread SpreadOf(const std::vector<double>& values)
{
    return {Percentile(values, 0.5), Percentile(values, 0.9)};
}

std::vector<double> Column(const std::vector<Measured>& measured, double Measured::* member)
{
    std::vector<double> values;
    for (const auto& item : measured) values.push_back(item.*member);
    return values;
}

std::vector<double> Ordinal(const std::vector<Measured>& measured, size_t ordinal)
{
    std::vector<double> values;
    for (const auto& item : measured) values.push_back(item.dispatchMs[ordinal]);
    return values;
}

struct Labels
{
    std::map<ID3D12PipelineState*, unsigned> passes;

    std::string Of(const Key& key)
    {
        if (key.layer)
        {
            const auto layers = xefg_cuda::Layers();
            const char* kernel = key.layer <= layers.size() ? layers[key.layer - 1].kernel : "?";
            return "L" + std::to_string(key.layer) + " " + kernel;
        }
        const auto found = passes.emplace(key.pipeline, static_cast<unsigned>(passes.size()) + 1);
        return "pass " + std::to_string(found.first->second);
    }

    void PrintPrograms() const
    {
        std::map<unsigned, ID3D12PipelineState*> ordered;
        for (const auto& [pipeline, number] : passes) ordered.emplace(number, pipeline);
        for (const auto& [number, pipeline] : ordered)
        {
            const auto program = ProgramOf(pipeline);
            std::printf("pass %u = %s %zu\n", number, program ? program->sha.c_str() : "?", program ? program->bytes : 0);
        }
    }
};

const Measured* FirstDifferent(const std::vector<Measured>& measured)
{
    for (const auto& item : measured)
        if (item.keys != measured.front().keys) return &item;
    return nullptr;
}

void PrintRows(const std::vector<Measured>& measured, Labels& labels)
{
    const auto& keys = measured.front().keys;
    for (size_t ordinal = 0; ordinal < keys.size(); ++ordinal)
    {
        const auto spread = SpreadOf(Ordinal(measured, ordinal));
        const auto& key = keys[ordinal];
        std::printf("  %3zu  %-34s%s %s  %ux%ux%u  median %7.3f  p90 %7.3f\n", ordinal, labels.Of(key).c_str(),
            ShaColumn(key.pipeline).c_str(), key.launched ? "y" : "n", key.x, key.y, key.z, spread.median, spread.p90);
    }
}

enum Category { kLaunched, kDeclined, kPass, kCategories };

Category CategoryOf(const Key& key)
{
    if (key.launched) return kLaunched;
    return key.layer ? kDeclined : kPass;
}

void PrintCategories(const std::vector<Measured>& measured)
{
    std::vector<double> sums[kCategories], tail;
    for (const auto& item : measured)
    {
        double total[kCategories]{};
        for (size_t i = 0; i < item.keys.size(); ++i) total[CategoryOf(item.keys[i])] += item.dispatchMs[i];
        for (int category = 0; category < kCategories; ++category) sums[category].push_back(total[category]);
        tail.push_back(item.tail);
    }
    std::printf("  tensor-core launches      median %7.3f ms\n", Percentile(sums[kLaunched], 0.5));
    std::printf("  layers fallen back to DXIL median %7.3f ms\n", Percentile(sums[kDeclined], 0.5));
    std::printf("  non-layer DXIL passes     median %7.3f ms\n", Percentile(sums[kPass], 0.5));
    std::printf("  after the last dispatch   median %7.3f ms\n", Percentile(tail, 0.5));
}

void PrintDispatches(const std::vector<Measured>& measured, Labels& labels)
{
    if (const auto* different = FirstDifferent(measured))
    {
        std::printf("  the dispatch sequence is not the same in every pair: pair %u differs from pair %u, "
                    "%zu against %zu dispatches; not aggregated\n",
            different->pair, measured.front().pair, different->keys.size(), measured.front().keys.size());
        return;
    }
    if (measured.front().keys.empty())
    {
        std::printf("  no dispatch went through the command-list proxy\n");
        return;
    }
    PrintRows(measured, labels);
    PrintCategories(measured);
}

void PrintPerRealFrame(const char* suffix, const PassResult& result)
{
    double total = 0, hud = 0;
    for (const auto& measured : result)
    {
        total += Percentile(Column(measured, &Measured::total), 0.5);
        hud += Percentile(Column(measured, &Measured::hud), 0.5);
    }
    std::printf("per real frame%s: execute %.3f ms, hud %.3f ms, total %.3f ms\n", suffix, total, hud, total + hud);
}

void PrintPass(const char* title, const char* suffix, bool rows, const PassResult& result, Labels& labels)
{
    std::printf("\n== %s ==\n", title);
    for (size_t frame = 0; frame < result.size(); ++frame)
    {
        const auto total = SpreadOf(Column(result[frame], &Measured::total));
        const auto hud = SpreadOf(Column(result[frame], &Measured::hud));
        std::printf("firstFrame %zu: execute median %.3f ms, p90 %.3f ms\n", frame, total.median, total.p90);
        if (rows) PrintDispatches(result[frame], labels);
        std::printf("  HUD composite median %.3f ms, p90 %.3f ms\n", hud.median, hud.p90);
    }
    PrintPerRealFrame(suffix, result);
}

void PrintHeader(const Bench& bench, const std::wstring& adapter, UINT64 frequency)
{
    const auto& o = bench.options;
    const char* route = o.dxil ? "off (--dxil)" : o.declined ? "installed, every layer declined (--declined)" : "tensor cores";
    std::printf("XmxXefgProfile\n");
    std::printf("adapter: %ls\n", adapter.c_str());
    std::printf("timestamp frequency: %llu Hz\n", static_cast<unsigned long long>(frequency));
    std::printf("output %ux%u R10G10B10A2, render %ux%u (depth R32_FLOAT, motion R16G16_FLOAT), generated frames %u\n",
        o.outputWidth, o.outputHeight, o.renderWidth, o.renderHeight, o.frames);
    std::printf("content %s\n", NameOf(o.content));
    if (bench.texel.read)
        std::printf("slot 0 motion texel at render (%u, %u) after the fill: (%.6f, %.6f), %.2f, %.2f render pixels\n",
            bench.texel.x, bench.texel.y, bench.texel.u, bench.texel.v, bench.texel.u * static_cast<float>(o.renderWidth),
            bench.texel.v * static_cast<float>(o.renderHeight));
    std::printf("pairs %u measured after %u warmup, %u pairs per command list, HUD composite %s, route %s\n", o.pairs,
        o.warmup, kPairsPerList, o.hud ? "on" : "off", route);
    std::printf("each interval is the completion of everything recorded before its stamp minus the completion of "
                "everything before the previous stamp:\n"
                "barriers, copies and clears between two dispatches are counted on the dispatch after them, and "
                "the first dispatch carries everything recorded since the execute began\n");
}

void PrintRoute(const char* when)
{
    const auto status = xefg_cuda::Snapshot();
    std::printf("route %s: state %d, layers %u, launches %llu, fallbacks %llu, reason %s\n", when,
        static_cast<int>(status.state), status.layers, static_cast<unsigned long long>(status.launches),
        static_cast<unsigned long long>(status.fallbacks), status.reason.c_str());
}

bool RouteReady(const Options& options)
{
    if (options.dxil) return true;
    const auto status = xefg_cuda::Snapshot();
    if (status.state == xefg_cuda::State::Active) return true;
    std::printf("FAIL the route is not active: state %d, reason %s\n", static_cast<int>(status.state),
        status.reason.c_str());
    return false;
}

bool CreateContext(Bench& bench)
{
    std::string failure;
    auto init = InitOf(bench.options);
    if (xmx::tensor_cores::Create(bench.fg, *bench.rvas, bench.gpu.device.Get(), bench.handle, failure) && bench.handle
        && xmx::tensor_cores::Initialize(bench.fg, *bench.rvas, bench.handle, init) == 0)
        return true;
    std::printf("FAIL the XeFG context is not created: %s\n", failure.c_str());
    return false;
}

UINT64 StartTiming(Bench& bench)
{
    bench.bases.assign(bench.options.frames * 2, 0);
    for (auto& output : bench.scene.outputs) bench.outputs.push_back(output.Get());
    UINT64 frequency = 0;
    bench.gpu.queue->GetTimestampFrequency(&frequency);
    bench.toMs = 1000.0 / static_cast<double>(frequency);
    return frequency;
}

bool Prepare(Bench& bench, HMODULE ll, const std::wstring& adapter)
{
    const auto& options = bench.options;
    std::string error;
    if (!xess_mod::PublishMultiFrameModel(bench.fg, ll, error))
    {
        std::printf("FAIL the multi-frame model does not publish: %s\n", error.c_str());
        return false;
    }
    if (!MakeScene(bench.gpu.device.Get(), options, bench.scene) || !MakeTimer(bench.gpu.device.Get(), bench.timer))
    {
        std::printf("FAIL the textures or the timestamp heap are not made\n");
        return false;
    }
    if (!FillScene(bench.gpu, options, bench.scene, bench.texel))
    {
        std::printf("FAIL the input textures are not filled with the %s content\n", NameOf(options.content));
        return false;
    }
    if (options.hud && !bench.hud.Prepare(bench.gpu.device.Get()))
    {
        std::printf("FAIL the HUD composite pass is not prepared: %s\n", bench.hud.Fault().c_str());
        return false;
    }
    if (!CreateContext(bench)) return false;
    if (options.declined) xefg_cuda::SetLayerMask(0);
    PrintHeader(bench, adapter, StartTiming(bench));
    return RouteReady(options);
}

bool LaunchesRecorded(const Options& options)
{
    if (options.dxil || options.declined || xefg_cuda::Snapshot().launches != 0) return true;
    std::printf("FAIL the run ended with zero tensor-core launches: the measurement is of the DXIL port\n");
    return false;
}

bool Profile(HMODULE fg, HMODULE ll, const xmx::Rvas& rvas, const Options& options)
{
    Bench bench;
    bench.options = options;
    bench.fg = fg;
    bench.rvas = &rvas;
    std::wstring adapter;
    bench.gpu.device = HardwareDevice(adapter);
    if (!bench.gpu.device || !MakeGpu(bench.gpu))
    {
        std::printf("SETUP no D3D12 hardware device at feature level 12_0\n");
        return false;
    }
    if (!DeviceIsAcceptable(options, bench.gpu.device.Get())) return false;
    if (!options.programs.empty() && !RecordPrograms(bench.gpu.device.Get(), options.programs))
    {
        std::printf("FAIL the pipeline-creation hooks are not installed or %ls is not writable\n", options.programs.c_str());
        return false;
    }
    if (options.dxil) xmx::tensor_cores::Configure(false);
    if (!Prepare(bench, ll, adapter)) return false;
    PrintRoute("after Initialize");
    Labels labels;
    PassResult plain, probed;
    if (!RunPass(bench, false, plain)) return false;
    PrintPass("execute and HUD composite stamped, no stamp inside the execute", "", false, plain, labels);
    if (!options.dxil)
    {
        if (!RunPass(bench, true, probed)) return false;
        PrintPass("one more stamp after every dispatch", " (stamp after every dispatch)", true, probed, labels);
        if (gBook.recording) labels.PrintPrograms();
    }
    std::printf("\n");
    PrintRoute("at the end");
    return LaunchesRecorded(options);
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

[[noreturn]] void ExitWithoutUnloading(int code)
{
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    std::abort();
}

int wmain(int argc, wchar_t** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        std::printf("usage: XmxXefgProfile <xess bin folder> <payload dll> [--output WxH] [--render WxH] [--frames N] "
                    "[--pairs N] [--warmup N] [--content zero|scene|noise] [--dxil | --declined] [--no-hud] "
                    "[--expect-wrapper] [--programs dir]\n");
        return 1;
    }
    wchar_t gate[8]{};
    if (GetEnvironmentVariableW(L"MFG_TEST_GPU", gate, 8) == 0 || gate[0] != L'1')
    {
        std::printf("SETUP creates a GPU device: set MFG_TEST_GPU=1 with the game closed\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    gPayload = LoadLibraryExW(options.payload.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const HMODULE ll =
        LoadLibraryExW((options.folder + L"\\libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const HMODULE fg =
        LoadLibraryExW((options.folder + L"\\libxess_fg.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!gPayload || !ll || !fg)
    {
        std::printf("FAIL the payload module, libxell.dll and libxess_fg.dll do not load\n");
        return 1;
    }
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
    if (!rvas)
    {
        std::printf("FAIL libxess_fg.dll 1.3.1.78 is not the supported fg78 family\n");
        return 1;
    }
    const int code = Profile(fg, ll, *rvas, options) ? 0 : 1;
    if (options.expectWrapper) ExitWithoutUnloading(code);
    return code;
}
