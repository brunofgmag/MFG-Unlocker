#include <cstdlib>
#include <iostream>

#include "xmx/low_latency_rules.h"

#define CHECK(condition)                                                                 \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
            std::abort();                                                                \
        }                                                                                \
    } while (false)

int main()
{
    using xmx::low_latency::Forwarded;
    using xmx::low_latency::HoldWanted;
    using xmx::low_latency::kReleased;
    using xmx::low_latency::Request;
    using xmx::low_latency::Resync;

    static_assert(HoldWanted(true, true));
    static_assert(!HoldWanted(true, false));
    static_assert(!HoldWanted(false, true));
    static_assert(!HoldWanted(false, false));

    static_assert(Forwarded({true, true, 500}, false) == Request{true, true, 500});
    static_assert(Forwarded({true, false, 0}, false) == Request{true, false, 0});
    static_assert(Forwarded({false, true, 7}, false) == Request{false, true, 7});
    static_assert(Forwarded({false, false, 0}, false) == Request{false, false, 0});
    static_assert(Forwarded({true, true, 500}, true) == Request{false, false, 500});
    static_assert(Forwarded({true, false, 0}, true) == Request{false, false, 0});
    static_assert(Forwarded({false, true, 7}, true) == Request{false, false, 7});
    static_assert(Forwarded({false, false, 0}, true) == Request{false, false, 0});

    for (const bool mode : {false, true})
        for (const bool boost : {false, true})
            for (const uint32_t interval : {0u, 1u, 8333u, 0xFFFFFFFFu})
            {
                const Request requested{mode, boost, interval};
                CHECK(Forwarded(requested, false) == requested);
                const Request held = Forwarded(requested, true);
                CHECK(!held.mode && !held.boost);
                CHECK(held.intervalUs == interval);
            }

    using Maybe = std::optional<Request>;
    const Request last{true, true, 8333};
    const Request held{false, false, 8333};
    const Request none{false, false, 0};
    CHECK(Resync(last, true) == Maybe(held));
    CHECK(Resync(last, false) == Maybe(last));
    CHECK(Resync(std::nullopt, true) == Maybe(kReleased));
    CHECK(kReleased == none);
    CHECK(!Resync(std::nullopt, false).has_value());
    CHECK(Resync(none, false) == Maybe(none));
    std::cout << "PASS low latency rules\n";
    return 0;
}
