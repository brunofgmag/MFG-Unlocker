#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "xmx/tag_states.h"

int gFailures = 0;

#define CHECK(condition)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(condition))                                                   \
        {                                                                   \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
            ++gFailures;                                                    \
        }                                                                   \
    } while (false)

int main()
{
    int depth = 0, motion = 0, other = 0;
    constexpr uint32_t fallback = 77;
    constexpr uint32_t unknown = UINT32_MAX;

    CHECK(xmx::tag_states::StateOf(&depth, fallback) == fallback);
    xmx::tag_states::Record(&depth, 0x40);
    xmx::tag_states::Record(&motion, 0x80);
    CHECK(xmx::tag_states::StateOf(&depth, fallback) == 0x40);
    CHECK(xmx::tag_states::StateOf(&motion, fallback) == 0x80);
    xmx::tag_states::Record(&depth, 0x10);
    CHECK(xmx::tag_states::StateOf(&depth, fallback) == 0x10);

    xmx::tag_states::Record(&depth, unknown);
    CHECK(xmx::tag_states::StateOf(&depth, fallback) == fallback);
    CHECK(xmx::tag_states::StateOf(&motion, fallback) == 0x80);

    xmx::tag_states::Record(&other, unknown);
    CHECK(xmx::tag_states::StateOf(&other, fallback) == fallback);
    xmx::tag_states::Record(nullptr, 0x40);
    xmx::tag_states::Record(nullptr, unknown);
    CHECK(xmx::tag_states::StateOf(nullptr, fallback) == fallback);

    xmx::tag_states::Record(&depth, 0x20);
    CHECK(xmx::tag_states::StateOf(&depth, fallback) == 0x20);

    if (gFailures != 0) return 1;
    std::cout << "PASS: tag states\n";
    return 0;
}
