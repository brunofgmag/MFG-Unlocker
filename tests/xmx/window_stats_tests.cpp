#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "main_thread_waits.h"
#include "window_stats.h"

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
    {
        std::vector<float> values;
        for (int i = 10; i >= 1; --i) values.push_back(static_cast<float>(i));
        const auto spread = window_stats::Summarize(values, 10);
        CHECK(spread.events == 10 && spread.median == 5.f && spread.p90 == 9.f && spread.max == 10.f);
        CHECK(window_stats::Summarize({7.f}, 1).median == 7.f && window_stats::Summarize({7.f}, 1).p90 == 7.f);
        const auto empty = window_stats::Summarize({}, 0);
        CHECK(empty.events == 0 && empty.median == 0.f && empty.p90 == 0.f && empty.max == 0.f);
    }

    {
        window_stats::Ring<8> ring;
        for (uint32_t i = 1; i <= 5; ++i) ring.Add(i * 1000);
        window_stats::Ring<8> untouched;
        CHECK(untouched.Drain().events == 0);
        const auto window = ring.Drain();
        CHECK(window.events == 5 && window.median == 3.f && window.p90 == 5.f && window.max == 5.f);
        CHECK(ring.Drain().events == 0);
        ring.Add(2000);
        ring.Add(4000);
        const auto next = ring.Drain();
        CHECK(next.events == 2 && next.median == 2.f && next.max == 4.f);
    }

    {
        window_stats::Ring<4> ring;
        for (uint32_t i = 1; i <= 10; ++i) ring.Add(i * 1000);
        const auto window = ring.Drain();
        CHECK(window.events == 10);
        CHECK(window.median == 8.f && window.max == 10.f);
        ring.Add(0);
        CHECK(ring.Drain().max == 0.001f);
    }

    {
        const window_stats::Spread present{300, 1.5f, 2.25f, 30.f}, reflex{299, 12.f, 20.5f, 40.f}, interval{300, 33.25f, 0.f, 0.f};
        CHECK(window_stats::WaitsLine(present, reflex, 301, 77, interval)
            == "Main-thread waits 10 s: present n=300 med=1.50 p90=2.25 max=30.00 ms; reflexSleep n=299 skipped=301 xell=77 "
               "med=12.00 p90=20.50 ms; frame interval med=33.25 ms");
    }

    {
        const int64_t frequency = main_thread_waits::Frequency();
        const int64_t interval = frequency / 30, wait = frequency / 100, sleep = frequency / 50;
        int64_t start = frequency;
        for (int i = 0; i < 5; ++i)
        {
            main_thread_waits::NotePresent(start, start + wait);
            start += interval;
        }
        main_thread_waits::NoteReflexSleep(frequency, frequency + sleep);
        CHECK(main_thread_waits::DrainLine()
            == "Main-thread waits 10 s: present n=5 med=10.00 p90=10.00 max=10.00 ms; reflexSleep n=1 skipped=0 xell=0 "
               "med=20.00 p90=20.00 ms; frame interval med=33.33 ms");
        CHECK(main_thread_waits::DrainLine().empty());
    }

    {
        for (int i = 0; i < 3; ++i) main_thread_waits::NoteReflexSkip();
        const std::string line = main_thread_waits::DrainLine();
        CHECK(line.find("reflexSleep n=0 skipped=3 xell=0 ") != std::string::npos);
        CHECK(main_thread_waits::DrainLine().empty());
    }

    {
        for (int i = 0; i < 4; ++i) main_thread_waits::NoteXellSleep();
        const std::string line = main_thread_waits::DrainLine();
        CHECK(line.find("reflexSleep n=0 skipped=0 xell=4 ") != std::string::npos);
        CHECK(main_thread_waits::DrainLine().empty());
    }

    if (gFailures != 0) return 1;
    std::cout << "PASS: window statistics\n";
    return 0;
}
