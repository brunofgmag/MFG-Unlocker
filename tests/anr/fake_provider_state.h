#pragma once
#include <Windows.h>
#include <nvsdk_ngx_defs.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

struct FakeProviderState
{
    unsigned inits = 0, creates = 0, evaluates = 0, releases = 0;
    unsigned long long applicationId = 0;
    wchar_t dataPath[MAX_PATH] = {};
    ID3D12Device* lastDevice = nullptr;
    ID3D12GraphicsCommandList* lastList = nullptr;
    NVSDK_NGX_Feature lastFeature = NVSDK_NGX_Feature_Reserved0;
    NVSDK_NGX_Parameter* lastParameters = nullptr;
    const NVSDK_NGX_Handle* lastCreated = nullptr;
    const NVSDK_NGX_Handle* lastEvaluated = nullptr;
    const NVSDK_NGX_Handle* lastReleased = nullptr;
    unsigned nextId = 9001;
    NVSDK_NGX_Result initResult = NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result createResult = NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result evaluateResult = NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result releaseResult = NVSDK_NGX_Result_Success;
};

using FakeProviderFn = FakeProviderState* (*)();
