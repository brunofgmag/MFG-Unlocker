#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>

namespace xmx::hud_composite
{
class Pass
{
public:
    bool Apply(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* output, ID3D12Resource* real,
        ID3D12Resource* hudless, bool debug) noexcept;
    bool Prepare(ID3D12Device* device) noexcept;
    bool BoundTo(ID3D12Device* device) const noexcept { return !device_ || device_.Get() == device; }
    std::array<Microsoft::WRL::ComPtr<IUnknown>, 3> Release() noexcept;
    bool Failed() const noexcept { return failed_; }
    const std::string& Fault() const noexcept { return fault_; }

private:
    bool Initialize(ID3D12Device* device) noexcept;
    bool Fail(const char* fault) noexcept;
    bool Record(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12Resource* output,
        ID3D12Resource* real, ID3D12Resource* hudless, bool debug);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT increment_{};
    uint32_t ring_{};
    bool failed_{};
    std::string fault_;
};
}
