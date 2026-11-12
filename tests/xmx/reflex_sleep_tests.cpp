#include <cstdlib>
#include <iostream>

#include "reflex_sleep.h"

#define CHECK(condition)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(condition))                                                   \
        {                                                                   \
            std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
            std::abort();                                                   \
        }                                                                   \
    } while (false)

int main()
{
    using reflex_sleep::Choose;
    using reflex_sleep::Route;
    static_assert(Choose(false, false, false) == Route::Driver);
    static_assert(Choose(false, false, true) == Route::Skip);
    static_assert(Choose(false, true, false) == Route::XmxXell);
    static_assert(Choose(false, true, true) == Route::XmxXell);
    static_assert(Choose(true, false, false) == Route::Xell);
    static_assert(Choose(true, false, true) == Route::Xell);
    static_assert(Choose(true, true, false) == Route::Xell);
    static_assert(Choose(true, true, true) == Route::Xell);
    CHECK(Choose(false, false, false) == Route::Driver);
    CHECK(Choose(false, false, true) == Route::Skip);
    CHECK(Choose(false, true, false) == Route::XmxXell);
    CHECK(Choose(false, true, true) == Route::XmxXell);
    CHECK(Choose(true, false, false) == Route::Xell);
    CHECK(Choose(true, false, true) == Route::Xell);
    CHECK(Choose(true, true, false) == Route::Xell);
    CHECK(Choose(true, true, true) == Route::Xell);

    CHECK(!reflex_sleep::Skipping());
    CHECK(!reflex_sleep::Configure(false));
    CHECK(!reflex_sleep::Skipping());
    CHECK(reflex_sleep::Configure(true));
    CHECK(reflex_sleep::Skipping());
    CHECK(!reflex_sleep::Configure(true));
    CHECK(reflex_sleep::Skipping());
    CHECK(reflex_sleep::Configure(false));
    CHECK(!reflex_sleep::Skipping());
    CHECK(!reflex_sleep::Configure(false));

    CHECK(!reflex_sleep::XmxXell());
    CHECK(!reflex_sleep::ConfigureXmxXell(false));
    CHECK(!reflex_sleep::XmxXell());
    CHECK(reflex_sleep::ConfigureXmxXell(true));
    CHECK(reflex_sleep::XmxXell());
    CHECK(!reflex_sleep::ConfigureXmxXell(true));
    CHECK(reflex_sleep::XmxXell());
    CHECK(!reflex_sleep::Skipping());
    CHECK(reflex_sleep::ConfigureXmxXell(false));
    CHECK(!reflex_sleep::XmxXell());
    CHECK(!reflex_sleep::ConfigureXmxXell(false));

    std::cout << "PASS: reflex sleep\n";
    return 0;
}
