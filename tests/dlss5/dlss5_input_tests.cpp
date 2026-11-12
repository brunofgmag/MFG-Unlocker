#include "dlss5.cpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
bool okay=true;
void Check(bool value,const char* name){std::printf("%s %s\n",value?"PASS":"FAIL",name);std::fflush(stdout);okay&=value;}

class Device final : public IUnknown {
public:
    ULONG references=1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** output) override { if(output)*output=nullptr;return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
};
ID3D12Device* AsDevice(Device& device){return reinterpret_cast<ID3D12Device*>(static_cast<IUnknown*>(&device));}

class Texture final : public ID3D12Resource {
public:
    Texture(const D3D12_RESOURCE_DESC& description,ID3D12Device* owner) : desc(description),device(owner) {}
    D3D12_RESOURCE_DESC desc;
    ID3D12Device* device;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** output) override { if(output)*output=nullptr;return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void** output) override {
        if(!output)return E_POINTER;
        *output=device;
        reinterpret_cast<IUnknown*>(device)->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Map(UINT,const D3D12_RANGE*,void**) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE Unmap(UINT,const D3D12_RANGE*) override {}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return 0; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT,const D3D12_BOX*,const void*,UINT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*,UINT,UINT,UINT,const D3D12_BOX*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS*) override { return E_NOTIMPL; }
};

D3D12_RESOURCE_DESC Description(DXGI_FORMAT format,UINT width,UINT height,UINT16 mips,D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=1;
    desc.MipLevels=mips;desc.Format=format;desc.SampleDesc.Count=1;desc.Flags=flags;
    return desc;
}
Device owner,other;
bool Accepted(const D3D12_RESOURCE_DESC& desc,bool viewed,ID3D12Device* asked=nullptr,D3D12_RESOURCE_DESC* seen=nullptr,bool anyMips=true) {
    Texture texture(desc,AsDevice(owner));
    D3D12_RESOURCE_DESC read{};
    const dlss5::Rect whole{0,0,static_cast<int32_t>(desc.Width),static_cast<int32_t>(desc.Height)};
    const bool accepted=dlss5::TextureOnDevice(&texture,asked?asked:AsDevice(owner),whole,read,viewed,anyMips);
    if(seen)*seen=read;
    return accepted;
}

void Textures() {
    constexpr auto kColorFlags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto here=AsDevice(owner);
    D3D12_RESOURCE_DESC seen{};
    Check(Accepted(Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,1,kColorFlags),true),
        "a single-mip color texture is accepted");
    Check(Accepted(Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,12,kColorFlags),true,nullptr,&seen)&&seen.MipLevels==12,
        "MSFS's 12-mip color texture is accepted and reported with its mips");
    Check(Accepted(Description(DXGI_FORMAT_R32_FLOAT,1920,1080,11,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),false),
        "MSFS's 11-mip depth texture is accepted");
    Check(Accepted(Description(DXGI_FORMAT_R16G16_FLOAT,1920,1080,6,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),false),
        "a mipped motion-vector texture is accepted");
    Check(!Accepted(Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,12,kColorFlags),true,nullptr,nullptr,false),
        "a mip chain is refused where the caller does not allow one");
    Check(!Accepted(Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,12,kColorFlags),true,AsDevice(other)),
        "a texture of another device is refused");
    Check(!Accepted(Description(DXGI_FORMAT_D32_FLOAT,3840,2160,12,kColorFlags),true),
        "a viewed texture without a typed view format is refused");
    Check(Accepted(Description(DXGI_FORMAT_D32_FLOAT,3840,2160,12,kColorFlags),false),
        "the same format is accepted where no view of it is made");
    auto buffer=Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,12,kColorFlags);buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    Check(!Accepted(buffer,true),"a buffer is refused");
    auto array=Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,12,kColorFlags);array.DepthOrArraySize=2;
    Check(!Accepted(array,true),"an array texture is refused");
    auto multisampled=Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,1,kColorFlags);multisampled.SampleDesc.Count=4;
    Check(!Accepted(multisampled,true),"a multisampled texture is refused");
    Check(!Accepted(Description(DXGI_FORMAT_R11G11B10_FLOAT,3840,2160,12,kColorFlags|D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),true),
        "a texture that denies shader resource views is refused");
    Texture narrow(Description(DXGI_FORMAT_R11G11B10_FLOAT,1280,720,12,kColorFlags),here);
    D3D12_RESOURCE_DESC read{};
    Check(!dlss5::TextureOnDevice(&narrow,here,{0,0,3840,2160},read,true),"a rectangle beyond the texture is refused");
    Check(owner.references==1&&other.references==1,"the device references taken by the checks were released");
}

bool Compiles(const char* source,size_t length,const char* entry) {
    ComPtr<ID3DBlob> binary,error;
    const HRESULT hr=D3DCompile(source,length,"MFGUnlocker DLSS 5",nullptr,nullptr,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&binary,&error);
    if(FAILED(hr)&&error)std::printf("     %s: %.300s\n",entry,static_cast<const char*>(error->GetBufferPointer()));
    return SUCCEEDED(hr);
}

void Shaders() {
    for(const char* entry:{"Encode","Decode","Copy","Meter"})
        Check(Compiles(dlss5::kShader,sizeof(dlss5::kShader)-1,entry),(std::string("kShader ")+entry+" compiles for cs_5_1").c_str());
    for(const char* entry:{"Hold","Carry","Compose","Fit","Solve"})
        Check(Compiles(dlss5::kBoostShader,sizeof(dlss5::kBoostShader)-1,entry),(std::string("kBoostShader ")+entry+" compiles for cs_5_1").c_str());
    Check(!strstr(dlss5::kShader,"isfinite")&&!strstr(dlss5::kBoostShader,"isfinite"),"neither shader uses isfinite, which Wine's compiler lacks in SM5");
}
}

int main(int argc,char** argv){
    const std::string mode=argc>1?argv[1]:"";
    if(mode=="textures")Textures();
    else if(mode=="shaders")Shaders();
    else {std::printf("FAIL unknown mode\n");return 2;}
    return okay?0:1;
}
