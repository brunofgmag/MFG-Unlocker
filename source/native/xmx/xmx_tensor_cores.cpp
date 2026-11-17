#include "xmx_tensor_cores.h"
#include "xmx_status.h"
#include "xess/cuda/provider_list.h"
#include "xess/xess_mod.h"
#include <atomic>
#include <memory>

namespace xmx::tensor_cores
{
namespace
{
std::atomic<bool> gWanted{true};
std::atomic<Setting> gSetting{Setting::Undecided};
std::atomic<bool> gRestartLogged{false};
std::atomic<ID3D12Device*> gInstalledOn{nullptr};
std::unique_ptr<xefg_cuda::ProviderList> gProxy;

Setting Decide() noexcept
{
    auto setting = gSetting.load(std::memory_order_acquire);
    if (setting != Setting::Undecided) return setting;
    setting = gWanted.load(std::memory_order_relaxed) ? Setting::On : Setting::Off;
    gSetting.store(setting, std::memory_order_release);
    return setting;
}

uint64_t Token() noexcept
{
    static const uint64_t token = xefg_cuda::NewPipelineToken();
    return token;
}

void InstallOn(HMODULE fg, ID3D12Device* device) noexcept
{
    if (gInstalledOn.exchange(device, std::memory_order_relaxed) != device)
        xess_mod::InstallMultiFrameTensorCores(fg, device);
}

void ProxyWhenActive() noexcept
{
    try
    {
        if (!gProxy && xefg_cuda::Snapshot().state == xefg_cuda::State::Active)
            gProxy = std::make_unique<xefg_cuda::ProviderList>();
    }
    catch (...) {}
}

int ExecuteProxied(HMODULE fg, const Rvas& rvas, void* handle, ID3D12GraphicsCommandList* list,
    ExecuteParams& execute) noexcept
{
    xefg_cuda::PipelineScope scope(Token());
    const bool proxied = gProxy->Bind(list);
    const int result = context::Execute(fg, rvas, handle, proxied ? gProxy->Interface() : list, execute);
    if (proxied)
    {
        gProxy->Unbind();
        if (result < 0) gProxy->Disable(result);
    }
    return result;
}
}

void Configure(bool on) noexcept
{
    gWanted.store(on, std::memory_order_relaxed);
    const auto setting = gSetting.load(std::memory_order_acquire);
    const bool installedOn = setting == Setting::On;
    if (setting == Setting::Undecided || installedOn == on) return;
    if (gRestartLogged.exchange(true, std::memory_order_relaxed)) return;
    try
    {
        Log(std::string("tensor cores stay ") + (installedOn ? "on" : "off")
            + " until the game restarts: xmxTensorCores " + (on ? "1" : "0") + " applies on the next launch");
    }
    catch (...) {}
}

bool Create(HMODULE fg, const Rvas& rvas, ID3D12Device* device, void*& handle, std::string& failure) noexcept
{
    if (Decide() == Setting::Off) return context::Create(fg, rvas, device, handle, failure);
    InstallOn(fg, device);
    xefg_cuda::PipelineScope scope(Token());
    return context::Create(fg, rvas, device, handle, failure);
}

int Initialize(HMODULE fg, const Rvas& rvas, void* handle, InitParams& init) noexcept
{
    if (Decide() == Setting::Off) return context::Initialize(fg, rvas, handle, init);
    xefg_cuda::PipelineScope scope(Token());
    const int result = context::Initialize(fg, rvas, handle, init);
    ProxyWhenActive();
    return result;
}

int Execute(HMODULE fg, const Rvas& rvas, void* handle, ID3D12GraphicsCommandList* list,
    ExecuteParams& execute) noexcept
{
    if (!gProxy) return context::Execute(fg, rvas, handle, list, execute);
    return ExecuteProxied(fg, rvas, handle, list, execute);
}

std::string StatusText(const xefg_cuda::Status& status, Setting setting)
{
    if (setting == Setting::Off) return "tensor cores off (xmxTensorCores 0)";
    const auto none = setting == Setting::Undecided ? "no context yet" : "route not installed";
    const auto reason = status.reason.empty() ? std::string(none) : status.reason;
    switch (status.state)
    {
    case xefg_cuda::State::Active:
        return "tensor cores active launches " + std::to_string(status.launches) + " fallbacks "
            + std::to_string(status.fallbacks);
    case xefg_cuda::State::Disabled:
        return "tensor cores off (" + reason + ")";
    case xefg_cuda::State::Unavailable:
        return "tensor cores unavailable (" + reason + ")";
    default:
        return "tensor cores not attempted (" + reason + ")";
    }
}

std::string SummaryText()
{
    return StatusText(xefg_cuda::Snapshot(), gSetting.load(std::memory_order_acquire));
}
}
