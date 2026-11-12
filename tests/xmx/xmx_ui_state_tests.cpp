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
}

void Rows()
{
    xess_ui::State state;
    Check(!xess_ui::XmxXellShown(state), "the row is hidden while XMX is not latched");
    state.xmxLatched = TRUE;
    Check(xess_ui::XmxXellShown(state), "the row shows while XMX is latched");
    state.optiScaler = TRUE;
    Check(!xess_ui::XmxXellShown(state), "the row is hidden while OptiScaler owns the controls");
    state.optiScaler = FALSE;
    for (const BOOL on : {FALSE, TRUE})
        for (const BOOL ready : {FALSE, TRUE})
        {
            state.xmxXell = on;
            state.xmxXellReady = ready;
            const bool waits = xess_ui::XmxXellWaiting(state);
            Check(waits == (on && !ready), "waiting is on without a ready context only");
        }
    state.xmxXell = TRUE;
    state.xmxXellReady = FALSE;
    state.xmxLatched = FALSE;
    Check(!xess_ui::XmxXellWaiting(state), "nothing waits while XMX is not latched");
    state.xmxLatched = TRUE;
    state.optiScaler = TRUE;
    Check(!xess_ui::XmxXellWaiting(state), "nothing waits while OptiScaler owns the controls");
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
}

int main()
{
    Reading();
    Rows();
    ModelCombo();
    AnrModel();
    return gChecks != 0 && gFailures == 0 ? 0 : 1;
}
