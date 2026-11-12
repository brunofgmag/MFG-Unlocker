#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include "fake_provider_state.h"

namespace
{
FakeProviderState gState;

struct FakeHandle
{
    unsigned id;
};
}

extern "C" FakeProviderState* FakeProvider()
{
    return &gState;
}

extern "C" void NRFG_PresentationControl()
{
}

extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(unsigned long long applicationId,
    const wchar_t* dataPath, ID3D12Device* device, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*)
{
    ++gState.inits;
    gState.applicationId = applicationId;
    gState.lastDevice = device;
    lstrcpynW(gState.dataPath, dataPath ? dataPath : L"", MAX_PATH);
    return gState.initResult;
}

extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* list,
    NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle)
{
    ++gState.creates;
    gState.lastList = list;
    gState.lastFeature = feature;
    gState.lastParameters = parameters;
    if (NVSDK_NGX_FAILED(gState.createResult))
    {
        *handle = nullptr;
        return gState.createResult;
    }
    auto* created = reinterpret_cast<NVSDK_NGX_Handle*>(new FakeHandle{gState.nextId++});
    gState.lastCreated = created;
    *handle = created;
    return gState.createResult;
}

extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback)
{
    ++gState.evaluates;
    gState.lastList = list;
    gState.lastEvaluated = handle;
    gState.lastParameters = parameters;
    return gState.evaluateResult;
}

extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    ++gState.releases;
    gState.lastReleased = handle;
    return gState.releaseResult;
}

extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl(ID3D12Device*,
    NVSDK_NGX_Parameter*)
{
    return NVSDK_NGX_Result_Success;
}
