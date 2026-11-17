#include "tensor_cores_stub.h"
#include "xmx/xmx_tensor_cores.h"

namespace xmx::tensor_cores
{
namespace stub
{
unsigned creates = 0, initializes = 0, executes = 0;
std::string summary;
}

void Configure(bool) noexcept
{
}

bool Create(HMODULE fg, const Rvas& rvas, ID3D12Device* device, void*& handle, std::string& failure) noexcept
{
    ++stub::creates;
    return context::Create(fg, rvas, device, handle, failure);
}

int Initialize(HMODULE fg, const Rvas& rvas, void* handle, InitParams& init) noexcept
{
    ++stub::initializes;
    return context::Initialize(fg, rvas, handle, init);
}

int Execute(HMODULE fg, const Rvas& rvas, void* handle, ID3D12GraphicsCommandList* list,
    ExecuteParams& execute) noexcept
{
    ++stub::executes;
    return context::Execute(fg, rvas, handle, list, execute);
}

std::string SummaryText()
{
    return stub::summary;
}
}
