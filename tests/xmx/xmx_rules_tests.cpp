#include <Windows.h>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "xmx/xmx_context.h"
#include "xmx/xmx_rules.h"

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
    CHECK(xmx::Select(false, false) == xmx::Selection::eOff);
    CHECK(xmx::Select(false, true) == xmx::Selection::eOff);
    CHECK(xmx::Select(true, false) == xmx::Selection::eOn);
    CHECK(xmx::Select(true, true) == xmx::Selection::eRefusedStandalone);
    CHECK(std::string_view(xmx::RefusalName(xmx::Selection::eRefusedStandalone)) == "xemfg_standalone");
    CHECK(std::string_view(xmx::RefusalName(xmx::Selection::eOn)).empty());
    CHECK(std::string_view(xmx::RefusalName(xmx::Selection::eOff)).empty());

    CHECK(xmx::ForGpu(xmx::Selection::eOn, true) == xmx::Selection::eRefusedTranslatedGpu);
    CHECK(xmx::ForGpu(xmx::Selection::eOn, false) == xmx::Selection::eOn);
    CHECK(xmx::ForGpu(xmx::Selection::eOff, true) == xmx::Selection::eOff);
    CHECK(xmx::ForGpu(xmx::Selection::eRefusedStandalone, true) == xmx::Selection::eRefusedStandalone);
    CHECK(xmx::ForGpu(xmx::Selection::eRefusedTranslatedGpu, false) == xmx::Selection::eRefusedTranslatedGpu);
    CHECK(std::string_view(xmx::RefusalName(xmx::Selection::eRefusedTranslatedGpu)) == "translated_gpu");

    const xmx::Rvas* supported = xmx::RvasFor("fg78");
    CHECK(supported != nullptr);
    CHECK(supported->factory == 0x1853B0 && supported->init == 0x1871C0);
    CHECK(supported->execute == 0x185800 && supported->contextVtable == 0xF45D48);
    CHECK(xmx::RvasFor("fg68") == nullptr);
    CHECK(xmx::RvasFor("fg95") == nullptr);
    CHECK(xmx::RvasFor("") == nullptr);
    CHECK(xmx::RvasFor("fg78 ") == nullptr);

    CHECK(xmx::kMaxFrames == 5);
    CHECK(sizeof(xmx::context::InitParams) == 80);
    CHECK(sizeof(xmx::context::ExecuteParams) == 0x260);
    CHECK(xmx::context::kFlagDepthInverted == 1 && xmx::context::kFlagMotionJittered == 16);

    if (gFailures != 0) return 1;
    std::cout << "PASS: XMX rules\n";
    return 0;
}
