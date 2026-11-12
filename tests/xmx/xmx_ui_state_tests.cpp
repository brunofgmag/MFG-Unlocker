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
                Check(xess_ui::XmxModelLocked(state) == (blocked && !saved),
                    "the model combo is locked only while refused and not saved on, so a saved XMX can go back to NVIDIA");
            }
}
}

int main()
{
    Reading();
    Rows();
    ModelCombo();
    return gChecks != 0 && gFailures == 0 ? 0 : 1;
}
