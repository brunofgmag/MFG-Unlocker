#include <cstdint>
#include <iostream>

#include "xess/cuda/buffer_views.h"

using xefg_cuda::BufferViews;

int gFailures = 0;

#define CHECK(condition)                                                                 \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
            ++gFailures;                                                                 \
        }                                                                                \
    } while (false)

namespace
{
constexpr uint64_t kScope = 7;
constexpr uint64_t kResource = 0x7000'0000'0000ull;
constexpr UINT kIncrement = 32;

D3D12_CPU_DESCRIPTOR_HANDLE Handle(SIZE_T slot) { return {0x1000 + slot * kIncrement}; }

D3D12_SHADER_RESOURCE_VIEW_DESC Typed(DXGI_FORMAT format, UINT64 first = 0)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = first;
    desc.Buffer.NumElements = 64;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC Structured(UINT stride, UINT64 first = 0)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = Typed(DXGI_FORMAT_UNKNOWN, first);
    desc.Buffer.StructureByteStride = stride;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC Texture()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    return desc;
}

bool Writes(BufferViews& views, uint64_t scope, SIZE_T slot, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, uint64_t resource = kResource)
{
    views.WriteView(scope, Handle(slot), &desc, [resource] { return resource; });
    uint64_t address{};
    return views.Find(Handle(slot), address);
}

uint64_t At(const BufferViews& views, SIZE_T slot)
{
    uint64_t address{};
    return views.Find(Handle(slot), address) ? address : 0;
}

void Recording()
{
    BufferViews views;
    CHECK(Writes(views, kScope, 0, Typed(DXGI_FORMAT_R32G32B32A32_UINT)));
    CHECK(At(views, 0) == kResource);
    CHECK(Writes(views, kScope, 1, Typed(DXGI_FORMAT_R32G32B32A32_UINT, 5)));
    CHECK(At(views, 1) == kResource + 5 * 16);
    CHECK(Writes(views, kScope, 2, Typed(DXGI_FORMAT_R32G32B32A32_FLOAT, 3)));
    CHECK(At(views, 2) == kResource + 3 * 16);
    CHECK(Writes(views, kScope, 3, Typed(DXGI_FORMAT_R32G32B32A32_SINT, 1)));
    CHECK(At(views, 3) == kResource + 16);
    CHECK(Writes(views, kScope, 4, Structured(16, 9)));
    CHECK(At(views, 4) == kResource + 9 * 16);
    CHECK(views.Size() == 5);
}

void FormatAndStrideRules()
{
    BufferViews views;
    CHECK(!Writes(views, kScope, 0, Typed(DXGI_FORMAT_R32_UINT)));
    CHECK(!Writes(views, kScope, 1, Typed(DXGI_FORMAT_R32G32_UINT)));
    CHECK(!Writes(views, kScope, 2, Typed(DXGI_FORMAT_R8G8B8A8_UNORM)));
    CHECK(!Writes(views, kScope, 3, Structured(4)));
    CHECK(!Writes(views, kScope, 4, Structured(32)));
    CHECK(!Writes(views, kScope, 5, Structured(0)));
    D3D12_SHADER_RESOURCE_VIEW_DESC raw = Typed(DXGI_FORMAT_R32_TYPELESS);
    raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    CHECK(!Writes(views, kScope, 6, raw));
    D3D12_SHADER_RESOURCE_VIEW_DESC rawWide = Typed(DXGI_FORMAT_R32G32B32A32_UINT);
    rawWide.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    CHECK(!Writes(views, kScope, 7, rawWide));
    CHECK(!Writes(views, kScope, 8, Texture()));
    CHECK(!Writes(views, kScope, 9, Typed(DXGI_FORMAT_R32G32B32A32_UINT), 0));
    CHECK(views.Size() == 0);
    views.WriteView(kScope, Handle(10), nullptr, [] { return kResource; });
    CHECK(views.Size() == 0);
}

void OverwriteErases()
{
    BufferViews views;
    const auto wide = Typed(DXGI_FORMAT_R32G32B32A32_UINT, 2);
    CHECK(Writes(views, kScope, 0, wide));
    CHECK(!Writes(views, kScope, 0, Texture()));
    CHECK(Writes(views, kScope, 1, wide));
    CHECK(!Writes(views, kScope, 1, Structured(4)));
    CHECK(Writes(views, kScope, 2, wide));
    views.WriteView(kScope, Handle(2), nullptr, [] { return kResource; });
    CHECK(At(views, 2) == 0);
    CHECK(Writes(views, kScope, 3, wide));
    views.Erase(kScope, Handle(3));
    CHECK(At(views, 3) == 0);
    CHECK(Writes(views, kScope, 4, wide));
    CHECK(Writes(views, kScope, 4, Typed(DXGI_FORMAT_R32G32B32A32_UINT, 8), kResource + 0x10000));
    CHECK(At(views, 4) == kResource + 0x10000 + 8 * 16);
}

void SimpleCopies()
{
    BufferViews views;
    CHECK(Writes(views, kScope, 0, Typed(DXGI_FORMAT_R32G32B32A32_UINT, 1)));
    CHECK(Writes(views, kScope, 1, Structured(16, 2)));
    CHECK(Writes(views, kScope, 10, Texture(), kResource) == false);
    CHECK(Writes(views, kScope, 21, Structured(16, 7)));
    views.CopySimple(kScope, Handle(20), Handle(0), 3, kIncrement);
    CHECK(At(views, 20) == kResource + 16);
    CHECK(At(views, 21) == kResource + 32);
    CHECK(At(views, 22) == 0);
    CHECK(At(views, 0) == kResource + 16 && At(views, 1) == kResource + 32);
}

void RangeCopies()
{
    BufferViews views;
    for (SIZE_T slot = 0; slot < 4; ++slot) CHECK(Writes(views, kScope, slot, Typed(DXGI_FORMAT_R32G32B32A32_UINT, slot + 1)));
    const D3D12_CPU_DESCRIPTOR_HANDLE destinations[] = {Handle(30), Handle(50)};
    const UINT destinationSizes[] = {1, 3};
    const D3D12_CPU_DESCRIPTOR_HANDLE sources[] = {Handle(0), Handle(2)};
    const UINT sourceSizes[] = {2, 2};
    views.CopyRanges(kScope, 2, destinations, destinationSizes, 2, sources, sourceSizes, kIncrement);
    CHECK(At(views, 30) == kResource + 16);
    CHECK(At(views, 50) == kResource + 32);
    CHECK(At(views, 51) == kResource + 48);
    CHECK(At(views, 52) == kResource + 64);
    const D3D12_CPU_DESCRIPTOR_HANDLE oneByOne[] = {Handle(60), Handle(62), Handle(64)};
    const D3D12_CPU_DESCRIPTOR_HANDLE oneSources[] = {Handle(3), Handle(1), Handle(40)};
    views.CopyRanges(kScope, 3, oneByOne, nullptr, 3, oneSources, nullptr, kIncrement);
    CHECK(At(views, 60) == kResource + 64);
    CHECK(At(views, 62) == kResource + 32);
    CHECK(At(views, 64) == 0);
    CHECK(Writes(views, kScope, 70, Typed(DXGI_FORMAT_R32G32B32A32_UINT, 9)));
    const D3D12_CPU_DESCRIPTOR_HANDLE overwrite[] = {Handle(70)};
    const D3D12_CPU_DESCRIPTOR_HANDLE empty[] = {Handle(41)};
    const UINT one[] = {1};
    views.CopyRanges(kScope, 1, overwrite, one, 1, empty, one, kIncrement);
    CHECK(At(views, 70) == 0);
    views.CopyRanges(kScope, 0, destinations, destinationSizes, 0, sources, sourceSizes, kIncrement);
    views.CopyRanges(kScope, 1, nullptr, nullptr, 1, sources, nullptr, kIncrement);
}

void OverlappingCopyReadsBeforeWriting()
{
    BufferViews views;
    for (SIZE_T slot = 0; slot < 3; ++slot) CHECK(Writes(views, kScope, slot, Typed(DXGI_FORMAT_R32G32B32A32_UINT, slot + 1)));
    views.CopySimple(kScope, Handle(1), Handle(0), 3, kIncrement);
    CHECK(At(views, 1) == kResource + 16 && At(views, 2) == kResource + 32 && At(views, 3) == kResource + 48);
}

void OutOfScope()
{
    BufferViews views;
    int resolved = 0;
    views.WriteView(0, Handle(0), nullptr, [&] { ++resolved; return kResource; });
    const auto wide = Typed(DXGI_FORMAT_R32G32B32A32_UINT);
    views.WriteView(0, Handle(0), &wide, [&] { ++resolved; return kResource; });
    CHECK(views.Size() == 0 && resolved == 0);
    CHECK(Writes(views, kScope, 1, wide));
    CHECK(Writes(views, kScope, 2, wide));
    CHECK(Writes(views, 0, 1, Texture()));
    CHECK(At(views, 1) == kResource);
    views.Erase(0, Handle(1));
    CHECK(At(views, 1) == kResource);
    views.CopySimple(0, Handle(5), Handle(1), 2, kIncrement);
    CHECK(At(views, 5) == 0 && At(views, 6) == 0);
    views.CopySimple(0, Handle(2), Handle(40), 1, kIncrement);
    CHECK(At(views, 2) == kResource);
    const D3D12_CPU_DESCRIPTOR_HANDLE destination[] = {Handle(8)};
    const D3D12_CPU_DESCRIPTOR_HANDLE source[] = {Handle(1)};
    views.CopyRanges(0, 1, destination, nullptr, 1, source, nullptr, kIncrement);
    CHECK(At(views, 8) == 0);
}

void ClearForgetsEveryAddress()
{
    BufferViews views;
    const auto wide = Typed(DXGI_FORMAT_R32G32B32A32_UINT);
    CHECK(Writes(views, kScope, 1, wide));
    CHECK(Writes(views, kScope, 2, wide, kResource + 64));
    CHECK(Writes(views, kScope, 9, wide, kResource + 128));
    CHECK(views.Size() == 3);
    views.Clear();
    uint64_t address{};
    CHECK(views.Size() == 0);
    CHECK(!views.Find(Handle(1), address) && !views.Find(Handle(2), address) && !views.Find(Handle(9), address));
    CHECK(Writes(views, kScope, 1, wide, kResource + 256));
    CHECK(At(views, 1) == kResource + 256 && views.Size() == 1);
}
}

int main()
{
    Recording();
    FormatAndStrideRules();
    OverwriteErases();
    SimpleCopies();
    RangeCopies();
    OverlappingCopyReadsBeforeWriting();
    OutOfScope();
    ClearForgetsEveryAddress();
    if (gFailures) return 1;
    std::cout << "buffer view tests passed\n";
    return 0;
}
