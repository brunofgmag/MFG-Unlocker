#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "xess_ui_state.h"

namespace
{
unsigned gChecks = 0, gFailures = 0;

void Check(bool ok, const char* text)
{
    ++gChecks;
    gFailures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
}

bool Find(const std::string& text, const char* key, std::string& value)
{
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const size_t begin = at + needle.size();
    const size_t end = text.find_first_of(",}", begin);
    value = text.substr(begin, end - begin);
    return true;
}

xess_ui::State Parse(const std::string& status)
{
    return xess_ui::Read(
        status,
        [](const std::string& text, const char* key, BOOL& out) {
            std::string value;
            if (!Find(text, key, value)) return false;
            out = value == "true" ? TRUE : FALSE;
            return true;
        },
        [](const std::string& text, const char* key, uint32_t& out) {
            std::string value;
            if (!Find(text, key, value)) return false;
            out = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            return true;
        },
        [](const std::string& text, const char* key, std::string& out) {
            std::string value;
            if (!Find(text, key, value)) return false;
            out = value;
            return true;
        });
}

void Reading()
{
    const auto absent = Parse("{\"xmxLatched\":true}");
    Check(!absent.xmxXell && !absent.xmxXellReady, "an older status without the keys reads XeLL as off and not ready");
    const auto off = Parse("{\"xmxXell\":false,\"xmxXellReady\":false}");
    Check(!off.xmxXell && !off.xmxXellReady, "XeLL off and not ready is read");
    const auto on = Parse("{\"xmxXell\":true,\"xmxXellReady\":false}");
    Check(on.xmxXell && !on.xmxXellReady, "XeLL on and not ready is read");
    const auto ready = Parse("{\"xmxXell\":false,\"xmxXellReady\":true}");
    Check(!ready.xmxXell && ready.xmxXellReady, "XeLL ready without the key on is read");
    const auto both = Parse("{\"xmxXell\":true,\"xmxXellReady\":true}");
    Check(both.xmxXell && both.xmxXellReady, "XeLL on and ready is read");
    Check(absent.xmxHud, "an older status without the key reads the HUD composite as on, the mod's default");
    Check(!Parse("{\"xmxHud\":false}").xmxHud, "the HUD composite off is read");
    Check(Parse("{\"xmxHud\":true,\"xmxHudDebug\":false}").xmxHud, "the HUD composite on is read, apart from its debug key");
    Check(Parse("{\"xmxHudDebug\":true,\"xmxHud\":false}").xmxHud == FALSE, "the debug key does not stand for the HUD composite");
}

void Rows()
{
    xess_ui::State state;
    Check(xess_ui::XellShown(state), "the row shows with NVIDIA's generator");
    state.anrLatched = TRUE;
    Check(xess_ui::XellShown(state), "the row shows with ANR FG");
    state.anrLatched = FALSE;
    state.xmxLatched = TRUE;
    Check(xess_ui::XellShown(state), "the row shows with XMX");
    state.optiScaler = TRUE;
    Check(!xess_ui::XellShown(state), "the row is hidden while OptiScaler owns the controls");
    state.optiScaler = FALSE;
    for (const BOOL latched : {FALSE, TRUE})
        for (const BOOL on : {FALSE, TRUE})
            for (const BOOL ready : {FALSE, TRUE})
            {
                state.xmxLatched = latched;
                state.xmxXell = on;
                state.xmxXellReady = ready;
                const bool waits = xess_ui::XellWaiting(state);
                Check(waits == (on && !ready), "waiting is on without a ready context only, with or without XMX");
            }
    state.xmxXell = TRUE;
    state.xmxXellReady = FALSE;
    state.optiScaler = TRUE;
    Check(!xess_ui::XellWaiting(state), "nothing waits while OptiScaler owns the controls");
    xess_ui::State hud;
    Check(!xess_ui::HudShown(hud), "the HUD composite row is hidden with NVIDIA's generator");
    hud.anrSaved = TRUE;
    Check(!xess_ui::HudShown(hud), "and with ANR FG");
    hud.anrSaved = FALSE;
    hud.xmxSaved = TRUE;
    Check(xess_ui::HudShown(hud), "it shows when XeMFG DLSS-G or FSR MFG is the saved model");
    hud.xmxModelSaved = 1;
    Check(xess_ui::HudShown(hud), "FSR MFG included");
}

void ModelCombo()
{
    for (const bool standalone : {false, true})
        for (const bool refused : {false, true})
            for (const bool saved : {false, true})
            {
                xess_ui::State state;
                state.xemfgStandalone = standalone;
                state.xmxRefusal = refused ? "translated_gpu" : "";
                state.xmxSaved = saved;
                const bool blocked = standalone || refused;
                Check(xess_ui::XmxRefused(state) == blocked, "XMX is refused by standalone or by a refusal");
                state.anrRefusal = refused ? "translated_gpu" : "";
                Check(xess_ui::ModelLocked(state) == (blocked && !saved),
                    "the model combo is locked only while both models are refused and none is saved, so a saved XMX "
                    "can go back to NVIDIA");
            }
}

void AnrModel()
{
    xess_ui::State state;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eNvidia, "no saved key shows NVIDIA");
    state.anrSaved = TRUE;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eAnr, "a saved anr key shows ANR FG");
    state.xmxSaved = TRUE;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eXmx, "XMX wins the combo when both keys are saved, as it does at the latch");
    state = {};
    Check(!xess_ui::ModelRefused(state, xess_ui::Model::eAnr), "ANR FG can be picked by default");
    state.anrRefusal = "xmx";
    Check(!xess_ui::AnrRefused(state), "XMX's refusal does not stop picking ANR FG, which turns XMX off");
    state.anrRefusal = "translated_gpu";
    Check(xess_ui::ModelRefused(state, xess_ui::Model::eAnr), "a translated GPU refuses picking ANR FG");
    state.anrRefusal.clear();
    state.xemfgStandalone = TRUE;
    Check(xess_ui::ModelRefused(state, xess_ui::Model::eAnr), "XeMFG standalone refuses picking ANR FG");
    Check(!xess_ui::ModelRefused(state, xess_ui::Model::eNvidia), "NVIDIA can always be picked");
    state = {};
    state.xmxRefusal = "translated_gpu";
    Check(!xess_ui::ModelLocked(state), "the combo stays open while ANR FG can still be picked");
    state = {};
    state.anrSaved = TRUE;
    Check(xess_ui::ModelRestartPending(state), "a saved ANR FG that is not latched waits for a restart");
    state.anrLatched = TRUE;
    Check(!xess_ui::ModelRestartPending(state), "a latched ANR FG waits for nothing");
    state = {};
    state.anrSaved = TRUE;
    state.xmxSaved = TRUE;
    state.xmxLatched = TRUE;
    state.anrRefusal = "xmx";
    Check(!xess_ui::ModelRestartPending(state), "ANR FG held off by a running XMX is not a pending restart");
    state = {};
    state.anrLatched = TRUE;
    state.anrFailure = "not found";
    Check(xess_ui::AnrFailed(state), "a latched ANR FG with a failure and no evaluate has failed");
    state.anrEvaluating = TRUE;
    Check(!xess_ui::AnrFailed(state), "an evaluating ANR FG has not failed");
    const auto read = Parse("{\"anrSaved\":true,\"anrLatched\":true,\"anrLoaded\":true,\"anrEvaluating\":false,"
        "\"anrLive\":2,\"anrRefusal\":xmx,\"anrFailure\":gone}");
    Check(read.anrSaved && read.anrLatched && read.anrLoaded && !read.anrEvaluating && read.anrLive == 2
            && read.anrRefusal == "xmx" && read.anrFailure == "gone", "the ANR fields are read from the status");
}

void FsrModel()
{
    xess_ui::State state;
    state.xmxSaved = TRUE;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eXmx, "a saved XMX with the XeFG model shows XeMFG DLSS-G");
    state.xmxModelSaved = 1;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eFsr, "a saved XMX with the FSR model shows FSR MFG");
    state.xmxSaved = FALSE;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eNvidia, "the FSR model key alone does not pick FSR MFG");
    state.anrSaved = TRUE;
    Check(xess_ui::SavedModel(state) == xess_ui::Model::eAnr, "the FSR model key alone leaves ANR FG as it was");

    state = {};
    Check(!xess_ui::ModelRefused(state, xess_ui::Model::eFsr), "FSR MFG can be picked by default");
    state.xemfgStandalone = TRUE;
    Check(xess_ui::ModelRefused(state, xess_ui::Model::eFsr), "XeMFG standalone refuses picking FSR MFG");
    state = {};
    state.xmxRefusal = "translated_gpu";
    Check(xess_ui::ModelRefused(state, xess_ui::Model::eFsr), "a translated GPU refuses picking FSR MFG");
    state = {};
    state.xmxSaved = TRUE;
    state.xmxModelSaved = 1;
    state.xmxRefusal = "translated_gpu";
    state.anrRefusal = "translated_gpu";
    Check(!xess_ui::ModelLocked(state), "a saved FSR MFG can go back to NVIDIA even when every model is refused");

    state = {};
    state.xmxSaved = TRUE;
    state.xmxLatched = TRUE;
    state.xmxModelSaved = 1;
    state.xmxModelInEffect = "xefg";
    Check(xess_ui::ModelRestartPending(state), "a saved FSR MFG over a running XeFG model waits for a restart");
    state.xmxModelInEffect = "fsr";
    Check(!xess_ui::ModelRestartPending(state), "a saved FSR MFG that is running waits for nothing");
    state.xmxModelInEffect.clear();
    Check(!xess_ui::ModelRestartPending(state), "an unnamed model in effect is not a pending restart");
    state.xmxModelSaved = 0;
    state.xmxModelInEffect = "fsr";
    Check(xess_ui::ModelRestartPending(state), "a saved XeFG model over a running FSR model waits for a restart");
    state.xmxModelInEffect = "xefg";
    Check(!xess_ui::ModelRestartPending(state), "a saved XeFG model that is running waits for nothing");
    state.xmxLatched = FALSE;
    state.xmxSaved = FALSE;
    state.xmxModelInEffect = "fsr";
    Check(!xess_ui::ModelRestartPending(state), "a model name without a latched XMX is not a pending restart");

    Check(xess_ui::ModelUsesXmx(xess_ui::Model::eXmx) && xess_ui::ModelUsesXmx(xess_ui::Model::eFsr),
        "both XeMFG DLSS-G models write the xmx key");
    Check(!xess_ui::ModelUsesXmx(xess_ui::Model::eNvidia) && !xess_ui::ModelUsesXmx(xess_ui::Model::eAnr),
        "NVIDIA and ANR FG do not write the xmx key");

    state = {};
    Check(std::string(xess_ui::XmxModelName(state)) == "XeMFG DLSS-G", "the generator is named XeMFG DLSS-G by default");
    state.xmxModelInEffect = "xefg";
    Check(std::string(xess_ui::XmxModelName(state)) == "XeMFG DLSS-G", "the XeFG model is named XeMFG DLSS-G");
    state.xmxModelInEffect = "fsr";
    Check(std::string(xess_ui::XmxModelName(state)) == "FSR MFG", "the FSR model is named FSR MFG");

    const auto absent = Parse("{\"xmxLatched\":true}");
    Check(absent.xmxModelSaved == 0 && absent.xmxModelInEffect.empty(), "an older status reads no model keys");
    const auto read = Parse("{\"xmxModelSaved\":1,\"xmxModel\":fsr}");
    Check(read.xmxModelSaved == 1 && read.xmxModelInEffect == "fsr", "the model fields are read from the status");
}
}

int main()
{
    Reading();
    Rows();
    ModelCombo();
    AnrModel();
    FsrModel();
    return gChecks != 0 && gFailures == 0 ? 0 : 1;
}
