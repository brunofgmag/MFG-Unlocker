#include "hud_pass_stub.h"
#include "xmx/xmx_hud_composite.h"

namespace xmx::hud_composite
{
namespace stub
{
bool prepare = true;
std::string fault;
}

bool Pass::Prepare(ID3D12Device*) noexcept
{
    fault_ = stub::fault;
    return stub::prepare;
}

std::array<Microsoft::WRL::ComPtr<IUnknown>, 3> Pass::Release() noexcept
{
    return {};
}

bool Pass::Apply(ID3D12GraphicsCommandList*, ID3D12Device*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
    bool) noexcept
{
    return true;
}
}
