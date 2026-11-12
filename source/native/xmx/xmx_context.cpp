#include "xmx_context.h"

namespace xmx::context
{
int Guarded(void* function, void* handle, void* first, void* second) noexcept
{
    using Call = int(__cdecl*)(void*, void*, void*);
    __try
    {
        return reinterpret_cast<Call>(function)(handle, first, second);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return kCrashed;
    }
}

namespace
{
bool CreateFromFactory(HMODULE fg, const Rvas& rvas, ID3D12Device* device, void*& handle, std::string& failure)
{
    handle = nullptr;
    auto* base = reinterpret_cast<unsigned char*>(fg);
    void** wrapper = nullptr;
    const int created = Guarded(base + rvas.factory, device, &wrapper, nullptr);
    if (created != 0 || wrapper == nullptr)
    {
        failure = "factory returned " + std::to_string(created);
        return false;
    }
    if (wrapper[1] == nullptr
        || reinterpret_cast<uintptr_t>(*static_cast<void***>(wrapper[1])) - reinterpret_cast<uintptr_t>(base)
            != rvas.contextVtable)
    {
        failure = "unexpected context layout";
        return false;
    }
    handle = wrapper;
    return true;
}
}

bool Create(HMODULE fg, const Rvas& rvas, ID3D12Device* device, void*& handle, std::string& failure) noexcept
{
    try
    {
        return CreateFromFactory(fg, rvas, device, handle, failure);
    }
    catch (...)
    {
        handle = nullptr;
        return false;
    }
}

int Initialize(HMODULE fg, const Rvas& rvas, void* handle, InitParams& params) noexcept
{
    return Guarded(reinterpret_cast<unsigned char*>(fg) + rvas.init, handle, &params, nullptr);
}

int Execute(HMODULE fg, const Rvas& rvas, void* handle, ID3D12GraphicsCommandList* list,
    ExecuteParams& params) noexcept
{
    return Guarded(reinterpret_cast<unsigned char*>(fg) + rvas.execute, handle, list, &params);
}
}
