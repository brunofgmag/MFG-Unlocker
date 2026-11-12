#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "control_json.h"

#define CHECK(condition)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(condition))                                                   \
        {                                                                   \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

namespace
{
bool Contains(const std::string& text, std::string_view piece)
{
    return text.find(piece) != std::string::npos;
}
}

int main()
{
    const std::string saved =
        "{\"followGame\":true,\"mode\":\"follow\",\"xemfgStandalone\":0,\"xmx\":1,\"reflexSleepSkip\":1,\"xmxXell\":1,\"xmxStandIn\":1,\"xemfgFrames\":4,\"version\":13}\n";
    const std::string rebuilt =
        "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,\"dlssgPreset\":2,\"version\":13}\n";

    std::string content = rebuilt;
    control_json::Carry(content, saved);
    CHECK(Contains(content, "\"xmx\":1"));
    CHECK(Contains(content, "\"xemfgStandalone\":0"));
    CHECK(Contains(content, "\"xemfgFrames\":4"));
    CHECK(Contains(content, "\"reflexSleepSkip\":1"));
    CHECK(Contains(content, "\"xmxXell\":1"));
    CHECK(Contains(content, "\"xmxStandIn\":1"));
    CHECK(content.back() == '\n' || content.back() == '}');

    std::string chosen = "{\"followGame\":true,\"xmx\":0,\"version\":13}\n";
    control_json::Carry(chosen, saved);
    CHECK(Contains(chosen, "\"xmx\":0") && !Contains(chosen, "\"xmx\":1"));

    std::string untouched = rebuilt;
    control_json::Carry(untouched, "{\"version\":13}\n");
    CHECK(untouched == rebuilt);

    bool listed = false;
    for (const char* name : control_json::kCarriedFields) listed |= std::string_view(name) == "xmx";
    CHECK(listed);

    bool skipListed = false;
    for (const char* name : control_json::kCarriedFields) skipListed |= std::string_view(name) == "reflexSleepSkip";
    CHECK(skipListed);

    bool xellListed = false;
    for (const char* name : control_json::kCarriedFields) xellListed |= std::string_view(name) == "xmxXell";
    CHECK(xellListed);

    bool standInListed = false;
    for (const char* name : control_json::kCarriedFields) standInListed |= std::string_view(name) == "xmxStandIn";
    CHECK(standInListed);

    std::cout << "PASS: control carry\n";
    return 0;
}
