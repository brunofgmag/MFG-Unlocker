#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

#include "xmx/xmx_stand_in.h"

#define CHECK(condition)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(condition))                                                   \
        {                                                                   \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

using xmx::stand_in::Handle;
using xmx::stand_in::Registry;

int main()
{
    Registry registry;
    CHECK(registry.Live() == 0);

    Handle* first = registry.Issue();
    Handle* second = registry.Issue();
    CHECK(first != nullptr && second != nullptr && first != second);
    CHECK(first->Id == 2000001);
    CHECK(second->Id == 2000002);
    CHECK(first->Id != 0 && second->Id != first->Id && second->Id > first->Id);
    CHECK(registry.Live() == 2);

    CHECK(registry.Owns(first) && registry.Owns(second));
    CHECK(!registry.Owns(nullptr));
    int foreign = 0;
    CHECK(!registry.Owns(&foreign));
    CHECK(!registry.Retire(&foreign));
    CHECK(!registry.Retire(nullptr));
    CHECK(registry.Live() == 2);

    unsigned int retiredId = 0;
    CHECK(registry.Retire(first, &retiredId));
    CHECK(retiredId == 2000001);
    CHECK(registry.Live() == 1);
    CHECK(!registry.Retire(first));
    CHECK(registry.Retained() == 2);
    CHECK(first->Id == 2000001);
    CHECK(registry.Owns(second));

    Handle* third = registry.Issue();
    CHECK(third != nullptr && third->Id == 2000003);
    CHECK(registry.Retire(second));
    CHECK(registry.Retire(third));
    CHECK(registry.Live() == 0);
    CHECK(registry.Retained() == 3);

    Registry full;
    std::array<Handle*, Registry::kLimit> issued{};
    for (auto& slot : issued)
    {
        slot = full.Issue();
        CHECK(slot != nullptr);
    }
    CHECK(Registry::kLimit == 16);
    CHECK(full.Live() == 16);
    CHECK(full.Issue() == nullptr);
    CHECK(full.Live() == 16);
    CHECK(full.Retire(issued[5]));
    Handle* again = full.Issue();
    CHECK(again != nullptr && again->Id == 2000017 && full.Owns(again));
    CHECK(full.Issue() == nullptr);

    const std::wstring answers = xmx::stand_in::ConfigLine(true, true);
    CHECK(answers.find(L"is answered without NVIDIA's generator") != std::wstring::npos);
    const std::wstring unlatched = xmx::stand_in::ConfigLine(true, false);
    CHECK(unlatched.find(L"answered") == std::wstring::npos);
    CHECK(unlatched.find(L"goes to NVIDIA's generator") != std::wstring::npos);
    CHECK(unlatched.find(L"not latched") != std::wstring::npos);
    const std::wstring off = xmx::stand_in::ConfigLine(false, true);
    CHECK(off.find(L"stand-in off") != std::wstring::npos && off.find(L"answered") == std::wstring::npos);
    CHECK(xmx::stand_in::ConfigLine(false, false) == off);
    CHECK(answers != unlatched && answers != off && unlatched != off);

    std::cout << "PASS: xmx stand-in registry\n";
    return 0;
}
