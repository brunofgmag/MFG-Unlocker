#include "xmx_hud_composite.h"
#include <d3dcompiler.h>
#include <cstring>

namespace xmx::hud_composite
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr char kShader[] = R"(
Texture2D<float4> Real : register(t0);
Texture2D<float4> Hudless : register(t1);
RWTexture2D<float4> Output : register(u0);
cbuffer Params : register(b0) { uint2 size; float tolerance; uint debug; };
bool Hud(int2 p)
{
    const float3 d = abs(Real[p].rgb - Hudless[p].rgb);
    return max(d.r, max(d.g, d.b)) > tolerance;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= size)) return;
    bool hud = false;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 q = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1);
            hud = hud || Hud(q);
        }
    if (debug != 0) { if (hud) Output[id.xy] = float4(1, 0, 1, 1); }
    else if (hud) Output[id.xy] = Real[id.xy];
}
)";

constexpr uint32_t kSlots = 64;
constexpr uint32_t kDescriptorsPerSlot = 3;
constexpr float kTolerance = 3.0f / 255.0f;

using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
    ID3DBlob**);

HMODULE SystemModule(const wchar_t* name) noexcept
{
    HMODULE module = GetModuleHandleW(name);
    if (!module) module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}

DXGI_FORMAT UavFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

const char* CompileShader(ComPtr<ID3DBlob>& code) noexcept
{
    const HMODULE compiler = SystemModule(L"d3dcompiler_47.dll");
    const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return "d3dcompiler_47.dll is missing";
    ComPtr<ID3DBlob> errors;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "xmx_hud_composite", nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) || !code)
        return "the shader did not compile";
    return nullptr;
}

const char* SerializeSignature(ComPtr<ID3DBlob>& blob) noexcept
{
    const HMODULE d3d12 = SystemModule(L"d3d12.dll");
    const auto serialize = d3d12
        ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!serialize) return "D3D12SerializeRootSignature is missing";
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = parameters;
    ComPtr<ID3DBlob> errors;
    if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) || !blob) return "root signature";
    return nullptr;
}

bool SameSize(const D3D12_RESOURCE_DESC& left, const D3D12_RESOURCE_DESC& right) noexcept
{
    return left.Width == right.Width && left.Height == right.Height;
}
}

bool Pass::Fail(const char* fault) noexcept
{
    failed_ = true;
    try
    {
        fault_ = fault;
    }
    catch (...) {}
    return false;
}

bool Pass::Initialize(ID3D12Device* device) noexcept
{
    if (pipeline_) return true;
    if (failed_) return false;
    if (device == nullptr) return Fail("no device");
    ComPtr<ID3DBlob> code, signature;
    if (const char* fault = CompileShader(code)) return Fail(fault);
    if (const char* fault = SerializeSignature(signature)) return Fail(fault);
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&root_))))
        return Fail("root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)))) return Fail("pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kDescriptorsPerSlot,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return Fail("descriptor heap");
    increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_ = device;
    return true;
}

bool Pass::Prepare(ID3D12Device* device) noexcept
{
    if (failed_ || !Initialize(device)) return false;
    return device_.Get() == device;
}

std::array<ComPtr<IUnknown>, 3> Pass::Release() noexcept
{
    std::array<ComPtr<IUnknown>, 3> objects;
    objects[0] = std::move(root_);
    objects[1] = std::move(pipeline_);
    objects[2] = std::move(heap_);
    *this = Pass{};
    return objects;
}

bool Pass::Apply(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* output,
    ID3D12Resource* real, ID3D12Resource* hudless, bool debug) noexcept
{
    if (failed_) return false;
    if (list == nullptr || output == nullptr || real == nullptr || hudless == nullptr) return false;
    try
    {
        return Record(list, device, output, real, hudless, debug);
    }
    catch (...)
    {
        return Fail("unexpected error");
    }
}

bool Pass::Record(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* output,
    ID3D12Resource* real, ID3D12Resource* hudless, bool debug)
{
    if (!Initialize(device)) return false;
    if (device_.Get() != device) return Fail("the device changed");
    const auto out = output->GetDesc(), from = real->GetDesc(), hud = hudless->GetDesc();
    if (!SameSize(out, from) || !SameSize(hud, from)) return false;
    const uint32_t slot = ring_++ % kSlots;
    auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * kDescriptorsPerSlot * increment_;
    gpu.ptr += static_cast<UINT64>(slot) * kDescriptorsPerSlot * increment_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = UavFormat(from.Format);
    device->CreateShaderResourceView(real, &srv, cpu);
    cpu.ptr += increment_;
    srv.Format = UavFormat(hud.Format);
    device->CreateShaderResourceView(hudless, &srv, cpu);
    cpu.ptr += increment_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = UavFormat(out.Format);
    device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pipeline_.Get());
    const uint32_t width = static_cast<uint32_t>(out.Width), height = out.Height;
    uint32_t constants[4] = {width, height, 0, debug ? 1u : 0u};
    const float tolerance = kTolerance;
    std::memcpy(&constants[2], &tolerance, sizeof(tolerance));
    list->SetComputeRoot32BitConstants(0, 4, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = output;
    list->ResourceBarrier(1, &barrier);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    list->ResourceBarrier(1, &barrier);
    return true;
}
}
