#include "buffer_views.h"
#include <vector>

namespace xefg_cuda
{
namespace
{
bool SixteenByteElements(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) noexcept
{
    if (desc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) return false;
    switch (desc.Format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return true;
    case DXGI_FORMAT_UNKNOWN:
        return desc.Buffer.StructureByteStride == kViewElementBytes;
    default:
        return false;
    }
}

struct RangeCursor
{
    const D3D12_CPU_DESCRIPTOR_HANDLE* starts;
    const UINT* sizes;
    UINT count, increment;
    UINT range{}, offset{};

    UINT SizeOf(UINT index) const noexcept { return sizes ? sizes[index] : 1; }
    bool More() noexcept
    {
        while (range < count && offset >= SizeOf(range)) { ++range; offset = 0; }
        return range < count;
    }
    SIZE_T Handle() const noexcept { return starts[range].ptr + SIZE_T(offset)*increment; }
    void Step() noexcept { ++offset; }
};
}

bool BufferViewAddress(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, uint64_t resourceAddress, uint64_t& address) noexcept
{
    if (desc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER || !resourceAddress || !SixteenByteElements(desc)) return false;
    address = resourceAddress + desc.Buffer.FirstElement*kViewElementBytes;
    return true;
}

void BufferViews::Erase(uint64_t scope, D3D12_CPU_DESCRIPTOR_HANDLE destination)
{
    if (!scope) return;
    std::lock_guard lock(mutex_);
    addresses_.erase(destination.ptr);
}

void BufferViews::Clear() noexcept
{
    std::lock_guard lock(mutex_);
    addresses_.clear();
}

bool BufferViews::Find(D3D12_CPU_DESCRIPTOR_HANDLE handle, uint64_t& address) const
{
    std::lock_guard lock(mutex_);
    const auto found = addresses_.find(handle.ptr);
    if (found == addresses_.end()) return false;
    address = found->second;
    return true;
}

size_t BufferViews::Size() const
{
    std::lock_guard lock(mutex_);
    return addresses_.size();
}

std::vector<BufferViews::Write> BufferViews::Plan(UINT destinationCount, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
    const UINT* destinationSizes, UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes,
    UINT increment) const
{
    RangeCursor to{destinations, destinationSizes, destinationCount, increment};
    RangeCursor from{sources, sourceSizes, sourceCount, increment};
    std::vector<Write> writes;
    for (; to.More() && from.More(); to.Step(), from.Step())
    {
        const auto found = addresses_.find(from.Handle());
        const bool present = found != addresses_.end();
        writes.push_back({to.Handle(), present, present ? found->second : 0});
    }
    return writes;
}

void BufferViews::CopyRanges(uint64_t scope, UINT destinationCount, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
    const UINT* destinationSizes, UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes,
    UINT increment)
{
    if (!scope || !destinations || !sources) return;
    std::lock_guard lock(mutex_);
    for (const Write& write : Plan(destinationCount, destinations, destinationSizes, sourceCount, sources, sourceSizes, increment))
    {
        if (write.present) addresses_[write.handle] = write.address;
        else addresses_.erase(write.handle);
    }
}

void BufferViews::CopySimple(uint64_t scope, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source,
    UINT count, UINT increment)
{
    CopyRanges(scope, 1, &destination, &count, 1, &source, &count, increment);
}
}
