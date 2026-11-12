#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "anr/anr_handles.h"
#include "anr/anr_rules.h"

namespace
{
unsigned gChecks = 0, gFailures = 0;

void Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
}

bool Same(const char* a, const char* b)
{
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

void Selection()
{
    using anr::Selection;
    Check(anr::Select(false, false, false) == Selection::eOff, "no anr key leaves NVIDIA's generator");
    Check(anr::Select(false, true, true) == Selection::eOff, "XMX and standalone alone never select ANR FG");
    Check(anr::Select(true, false, false) == Selection::eOn, "the anr key alone selects ANR FG");
    Check(anr::Select(true, true, false) == Selection::eRefusedXmx, "XMX on refuses ANR FG");
    Check(anr::Select(true, false, true) == Selection::eRefusedStandalone, "XeMFG standalone refuses ANR FG");
    Check(anr::Select(true, true, true) == Selection::eRefusedStandalone, "standalone is the reason when both refuse");
    Check(anr::ForGpu(Selection::eOn, true) == Selection::eRefusedTranslatedGpu, "a translated GPU refuses ANR FG");
    Check(anr::ForGpu(Selection::eOn, false) == Selection::eOn, "a native GPU keeps ANR FG");
    Check(anr::ForGpu(Selection::eRefusedXmx, true) == Selection::eRefusedXmx, "an earlier refusal keeps its reason");
    Check(anr::ForGpu(Selection::eOff, true) == Selection::eOff, "off stays off on a translated GPU");
    Check(Same(anr::RefusalName(Selection::eRefusedXmx), "xmx"), "the XMX refusal is named xmx");
    Check(Same(anr::RefusalName(Selection::eRefusedStandalone), "xemfg_standalone"),
        "the standalone refusal keeps XMX's name for it");
    Check(Same(anr::RefusalName(Selection::eRefusedTranslatedGpu), "translated_gpu"),
        "the translated GPU refusal keeps XMX's name for it");
    Check(Same(anr::RefusalName(Selection::eOn), "") && Same(anr::RefusalName(Selection::eOff), ""),
        "on and off have no refusal");
}

void Identity()
{
    anr::Exports full{true, true, true, true, true, false};
    Check(anr::ProviderRefusal(full) == nullptr, "ANR FG's export set is accepted");
    anr::Exports nvidia = full;
    nvidia.nvidiaProvider = true;
    Check(Same(anr::ProviderRefusal(nvidia), "it is NVIDIA's DLSS-G, not ANR FG"), "NVIDIA's DLSS-G is refused");
    anr::Exports anonymous = full;
    anonymous.identity = false;
    Check(Same(anr::ProviderRefusal(anonymous), "it lacks ANR FG's NRFG_PresentationControl export"),
        "a provider without ANR FG's own export is refused");
    for (bool anr::Exports::* member : {&anr::Exports::init, &anr::Exports::create, &anr::Exports::evaluate,
             &anr::Exports::release})
    {
        anr::Exports partial = full;
        partial.*member = false;
        Check(Same(anr::ProviderRefusal(partial),
                  "it lacks NGX's D3D12 Init_Ext, CreateFeature, EvaluateFeature or ReleaseFeature"),
            "a provider missing one NGX entry is refused");
    }
}

void Candidates()
{
    const auto both = anr::Candidates(L"C:\\mod", L"C:\\game");
    Check(both.size() == 2, "the module's and the game's folders are both searched");
    Check(both.size() == 2 && both[0] == std::filesystem::path(L"C:\\mod\\RTXMFG-ANR\\nvngx_dlssg.dll"),
        "the module's RTXMFG-ANR folder comes first");
    Check(both.size() == 2 && both[1] == std::filesystem::path(L"C:\\game\\RTXMFG-ANR\\nvngx_dlssg.dll"),
        "the game's RTXMFG-ANR folder comes second");
    Check(anr::Candidates(L"C:\\Game", L"c:\\game").size() == 1, "one folder in two spellings is searched once");
    Check(anr::Candidates(L"", L"C:\\game").size() == 1, "an unknown module folder is skipped");
    Check(anr::Candidates(L"", L"").empty(), "no folder gives no candidate");
    Check(anr::LoadedCopy(L"C:\\mod\\RTXMFG-ANR\\nvngx_dlssg.dll")
            == std::filesystem::path(L"C:\\mod\\RTXMFG-ANR\\anr_fg_provider.dll"),
        "the loaded copy sits beside ANR FG's DLL under a name that is not NVIDIA's");
}

void Handles()
{
    anr::Handles handles;
    int a = 0, b = 0, foreign = 0;
    Check(!handles.Add(nullptr) && handles.Live() == 0, "a null handle is never tracked");
    Check(handles.Add(&a) && handles.Add(&b) && handles.Live() == 2, "two handles are tracked");
    Check(handles.Add(&a) && handles.Live() == 2, "a handle added twice is tracked once");
    Check(handles.Owns(&a) && !handles.Owns(&foreign) && !handles.Owns(nullptr), "only tracked handles are owned");
    Check(handles.Remove(&a) && !handles.Owns(&a) && handles.Live() == 1, "a removed handle is no longer owned");
    Check(!handles.Remove(&a) && !handles.Remove(&foreign), "a handle is removed once, a foreign one never");
    int many[anr::kHandleLimit] = {};
    unsigned added = 0;
    for (int& slot : many) added += handles.Add(&slot);
    Check(added == anr::kHandleLimit - 1 && handles.Live() == anr::kHandleLimit, "the limit fills the free slots");
    Check(!handles.Add(&foreign), "a handle beyond the limit is refused");
    Check(handles.Remove(&b) && handles.Add(&foreign), "a removal makes room again");
}
}

int main()
{
    Selection();
    Identity();
    Candidates();
    Handles();
    std::printf("ANR_RULES checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures || gChecks == 0 ? 1 : 0;
}
