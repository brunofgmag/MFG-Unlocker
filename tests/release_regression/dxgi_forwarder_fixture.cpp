#include <Windows.h>
namespace {
HMODULE SystemDxgi() noexcept {
    static const HMODULE module=[]{
        wchar_t path[MAX_PATH]{};
        const UINT length=GetSystemDirectoryW(path,MAX_PATH);
        if(!length||length>=MAX_PATH||wcscat_s(path,L"\\dxgi.dll"))return HMODULE{};
        return LoadLibraryW(path);
    }();
    return module;
}
template<class Function> Function Resolve(const char* name) noexcept {
    const HMODULE module=SystemDxgi();
    return module?reinterpret_cast<Function>(GetProcAddress(module,name)):nullptr;
}
}
extern "C" {
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID iid,void** output) {
    const auto entry=Resolve<HRESULT(WINAPI*)(REFIID,void**)>("CreateDXGIFactory");
    return entry?entry(iid,output):E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID iid,void** output) {
    const auto entry=Resolve<HRESULT(WINAPI*)(REFIID,void**)>("CreateDXGIFactory1");
    return entry?entry(iid,output):E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags,REFIID iid,void** output) {
    const auto entry=Resolve<HRESULT(WINAPI*)(UINT,REFIID,void**)>("CreateDXGIFactory2");
    return entry?entry(flags,iid,output):E_FAIL;
}
}
