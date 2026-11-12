#pragma once
#include <nvsdk_ngx_defs.h>

struct XmxFake
{
    bool latched = false;
    bool handles = false;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
    int evaluateCalls = 0;
    int createCalls = 0;
    void* list = nullptr;
    const void* parameters = nullptr;
    bool answersCreate = false;
    NVSDK_NGX_Result createResult = NVSDK_NGX_Result_Success;
    NVSDK_NGX_Handle* createHandle = nullptr;
    void* createList = nullptr;
    bool answersRelease = false;
    NVSDK_NGX_Result releaseResult = NVSDK_NGX_Result_Success;
    int releaseCalls = 0;
    const void* released = nullptr;
    int runtimeLayerCalls = 0;
    int providerLayerCalls = 0;
    int routesInstalled = 0;
    int routesFailed = 0;
};

struct GateFake
{
    bool pullMaskRestart = false;
    bool providerRefused = false;
    int beforeEvaluateCalls = 0;
    int beforeCreateCalls = 0;
};

extern XmxFake gXmx;
extern GateFake gGate;
