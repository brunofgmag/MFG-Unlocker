#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xefg_cuda
{
inline constexpr uint32_t kViewElementBytes = 16;

bool BufferViewAddress(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, uint64_t resourceAddress, uint64_t& address) noexcept;

class BufferViews
{
public:
    template <class ResourceAddress>
    void WriteView(uint64_t scope, D3D12_CPU_DESCRIPTOR_HANDLE destination, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
        ResourceAddress&& resourceAddress)
    {
        if (!scope) return;
        uint64_t address{};
        const bool buffer = desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER;
        std::lock_guard lock(mutex_);
        if (buffer && BufferViewAddress(*desc, resourceAddress(), address)) addresses_[destination.ptr] = address;
        else addresses_.erase(destination.ptr);
    }
    void Erase(uint64_t scope, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CopySimple(uint64_t scope, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source, UINT count,
        UINT increment);
    void CopyRanges(uint64_t scope, UINT destinationCount, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
        const UINT* destinationSizes, UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes,
        UINT increment);
    void Clear() noexcept;
    bool Find(D3D12_CPU_DESCRIPTOR_HANDLE handle, uint64_t& address) const;
    size_t Size() const;
private:
    struct Write { SIZE_T handle; bool present; uint64_t address; };
    std::vector<Write> Plan(UINT destinationCount, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations, const UINT* destinationSizes,
        UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes, UINT increment) const;
    mutable std::mutex mutex_;
    std::unordered_map<SIZE_T, uint64_t> addresses_;
};
}
