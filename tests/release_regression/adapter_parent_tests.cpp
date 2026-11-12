#include "overlay_adapter_parent.cpp"
#include "beside_executable.h"
#include <cstdio>
#include <string>
#include <vector>

namespace single_module {
std::vector<std::wstring> logged;
bool OwnsBackend() noexcept { return true; }
void Log(const wchar_t* text) noexcept { logged.emplace_back(text); }
}
namespace single_overlay::proxy {
unsigned routed=0;
HRESULT ParentFactoryCall(ParentFn original,IDXGIObject* object,REFIID iid,void** output) noexcept {
    ++routed;
    return original(object,iid,output);
}
}

namespace parent=single_overlay::adapter_parent;
namespace {
bool okay=true;
unsigned nativeParents=0;
void Check(bool value,const char* name){std::printf("%s %s\n",value?"PASS":"FAIL",name);std::fflush(stdout);okay&=value;}
bool Armed(){
    for(const auto& line:single_module::logged)if(line.find(L"adapter-parent entry armed")!=std::wstring::npos)return true;
    return false;
}
class Adapter final : public IDXGIAdapter {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIAdapter)){
            *output=static_cast<IDXGIAdapter*>(this);return S_OK;
        }
        *output=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID,void** output) override {
        ++nativeParents;
        if(output)*output=nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT,IDXGIOutput**) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID,LARGE_INTEGER*) override { return E_NOTIMPL; }
};
struct HeapTable { void** table; };
}

int main(int argc,char** argv){
    const volatile auto keepDxgiImported=&CreateDXGIFactory;(void)keepDxgiImported;
    if(argc>1&&!strcmp(argv[1],"local-dxgi")){
        Check(BesideExecutable(L"dxgi.dll"),"the process runs on a dxgi.dll beside the executable");if(!okay)return 1;
    }
    Check(GetModuleHandleW(L"dxgi.dll")!=nullptr,"a dxgi.dll is loaded");
    Check(HookMitigationsSupported(),"hook mitigations allow the public-entry detour");if(!okay)return 1;
    const HMODULE fixture=LoadLibraryW(L"ScopedCallerFixture.dll");
    using Foreign=HRESULT(WINAPI*)(IDXGIObject*,REFIID,void**);
    const auto foreign=fixture?reinterpret_cast<Foreign>(GetProcAddress(fixture,"ForeignParent")):nullptr;
    Check(foreign!=nullptr,"foreign caller fixture loads");if(!okay)return 1;

    Adapter adapter;
    IDXGIAdapter* const object=&adapter;
    auto** const table=*reinterpret_cast<void***>(&adapter);
    MEMORY_BASIC_INFORMATION owner{};
    VirtualQuery(table[6],&owner,sizeof(owner));
    Check(owner.AllocationBase==GetModuleHandleW(nullptr)&&GetModuleHandleW(L"dxgi.dll")!=GetModuleHandleW(nullptr),
        "the adapter's methods live in the executable, not in dxgi.dll");

    auto** const heapTable=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    memcpy(heapTable,table,10*sizeof(void*));
    HeapTable heap{heapTable};
    parent::Observe(reinterpret_cast<IUnknown*>(&heap));
    Check(!Armed()&&parent::observed==0,"an adapter whose method table is private memory is refused");

    parent::Observe(object);
    Check(Armed()&&parent::observed==1,"an adapter outside the system dxgi.dll arms the parent route");

    void* output=nullptr;
    const unsigned beforeNative=nativeParents;
    Check(object->GetParent(__uuidof(IDXGIFactory),&output)==E_NOINTERFACE&&nativeParents==beforeNative+1,
        "the executable's call reaches the native method");
    Check(single_overlay::proxy::routed==1&&parent::parentCalls==1,
        "the executable's call goes through the parent route");
    const unsigned routedBefore=single_overlay::proxy::routed;
    Check(foreign(object,__uuidof(IDXGIFactory),&output)==E_NOINTERFACE&&nativeParents==beforeNative+2
        &&single_overlay::proxy::routed==routedBefore,"a foreign DLL's call stays native");
    return okay?0:1;
}
