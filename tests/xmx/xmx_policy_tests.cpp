// SPDX-License-Identifier: MIT
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "xmx/xmx_policy.h"

static int gFailures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      ++gFailures;                                                              \
    }                                                                           \
  } while (false)

using namespace xmx::policy;

int main() {
  {
    auto texture = std::make_shared<int>(1);
    RetireQueue<std::shared_ptr<int>> queue;
    queue.Push(texture, 100);
    queue.Collect(100 + kRetireAfterPairs - 1);
    CHECK(queue.Size() == 1 && texture.use_count() == 2);
    queue.Collect(100 + kRetireAfterPairs);
    CHECK(queue.Size() == 0 && texture.use_count() == 1);
    for (uint64_t pair = 0; pair < 1000; ++pair) {
      queue.Collect(pair);
      for (int i = 0; i < 4; ++i) queue.Push(texture, pair);
    }
    CHECK(queue.Size() <= 4 * kRetireAfterPairs);
  }

  int deviceA = 0, deviceB = 0;
  const ContextShape shape{&deviceA, 3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM, 1};
  ContextShape same = shape;
  CHECK(shape == same);
  same.device = &deviceB;
  CHECK(!(shape == same));

  CHECK(!PairIsStale(0, 100000));
  CHECK(!PairIsStale(1000, 1000 + kStalePairMs));
  CHECK(PairIsStale(1000, 1000 + kStalePairMs + 1));

  auto plan = PlanFrames(3, 2, 5);
  CHECK(plan.count == 3 && plan.index == 2 && !plan.repeat && !plan.limited);
  plan = PlanFrames(3, 1, 1);
  CHECK(plan.count == 1 && plan.index == 1 && !plan.repeat && plan.limited);
  plan = PlanFrames(3, 3, 1);
  CHECK(plan.count == 1 && plan.index == 1 && plan.repeat && plan.limited);
  plan = PlanFrames(0, 0, 5);
  CHECK(plan.count == 1 && plan.index == 1 && !plan.repeat && !plan.limited);
  plan = PlanFrames(2, 9, 5);
  CHECK(plan.index == 2 && !plan.repeat);

  CHECK(CanCopy(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM, 1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM));
  CHECK(CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
  CHECK(CanCopy(1920, 1080, DXGI_FORMAT_R11G11B10_FLOAT, 1920, 1080, DXGI_FORMAT_R11G11B10_FLOAT));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT));
  CHECK(!CanCopy(1920, 1080, DXGI_FORMAT_R11G11B10_FLOAT, 1920, 1080, DXGI_FORMAT_R9G9B9E5_SHAREDEXP));

  CHECK(Bypassed(true, false, true));
  CHECK(!Bypassed(true, true, true));
  CHECK(!Bypassed(true, false, false));
  CHECK(!Bypassed(false, false, true));

  CHECK((InputArea(1488, 836, {0, 0, 1485, 835}) == Area{0, 0, 1485, 835}));
  CHECK((InputArea(1488, 836, {0, 0, 0, 0}) == Area{0, 0, 1488, 836}));
  CHECK((InputArea(1488, 836, {4, 0, 1485, 836}) == Area{0, 0, 1488, 836}));
  CHECK((InputArea(1488, 836, {0xFFFFFFFF, 0, 2, 836}) == Area{0, 0, 1488, 836}));
  auto [depth, motion] = SharedArea(InputArea(1488, 836, {}), InputArea(1485, 836, {}));
  CHECK((depth == Area{0, 0, 1485, 836} && motion == Area{0, 0, 1485, 836}));
  auto [offsetDepth, offsetMotion] = SharedArea({8, 4, 1480, 830}, {0, 0, 1485, 836});
  CHECK((offsetDepth == Area{8, 4, 1480, 830} && offsetMotion == Area{0, 0, 1480, 830}));

  CHECK(PickCopy(1, 1, 1, 1) == CopyMode::eWhole);
  CHECK(PickCopy(1, 1, 11, 1) == CopyMode::eSubresourceZero);
  CHECK(PickCopy(1, 1, 1, 6) == CopyMode::eSubresourceZero);
  CHECK(PickCopy(1, 1, 11, 6) == CopyMode::eSubresourceZero);
  CHECK(PickCopy(11, 1, 1, 1) == CopyMode::eSubresourceZero);
  CHECK(PickCopy(1, 4, 1, 1) == CopyMode::eSubresourceZero);
  CHECK(PickCopy(0, 0, 1, 1) == CopyMode::eWhole);

  CHECK(IsTypedDepth(DXGI_FORMAT_D32_FLOAT) && IsTypedDepth(DXGI_FORMAT_D16_UNORM));
  CHECK(IsTypedDepth(DXGI_FORMAT_D24_UNORM_S8_UINT) && IsTypedDepth(DXGI_FORMAT_D32_FLOAT_S8X24_UINT));
  CHECK(!IsTypedDepth(DXGI_FORMAT_R32_FLOAT) && !IsTypedDepth(DXGI_FORMAT_R32_TYPELESS));
  CHECK(!IsTypedDepth(DXGI_FORMAT_R10G10B10A2_UNORM));

  {
    const std::array<uint64_t, 5> stamps{1000, 1500, 2000, 5000, 5500};
    const auto cost = CostOfStamps(stamps, 1000000);
    CHECK(cost && std::fabs(cost->executeMs - 3.0f) < 1e-4f && std::fabs(cost->copyMs - 1.5f) < 1e-4f);
    CHECK(!CostOfStamps({0, 1500, 2000, 5000, 5500}, 1000000));
    CHECK(!CostOfStamps({1000, 1500, 2000, 5000, 0}, 1000000));
    CHECK(!CostOfStamps({1000, 1500, 1400, 5000, 5500}, 1000000));
    CHECK(!CostOfStamps(stamps, 0));
  }

  {
    PairCost pair;
    CHECK(!pair.Add(1, 3, 1.0f, 0.5f, true));
    CHECK(!pair.Add(2, 3, 1.5f, 0.25f, true));
    const auto frame = pair.Add(3, 3, 2.0f, 0.25f, true);
    CHECK(frame && frame->executes == 3);
    CHECK(frame && std::fabs(frame->executeMs - 4.5f) < 1e-4f && std::fabs(frame->copyMs - 1.0f) < 1e-4f);
    CHECK(!pair.Add(1, 3, 9.0f, 9.0f, true));
    CHECK(!pair.Add(3, 3, 9.0f, 9.0f, true));
    CHECK(!pair.Add(1, 3, 1.0f, 0.0f, true));
    CHECK(!pair.Add(2, 3, 1.0f, 0.0f, false));
    const auto next = pair.Add(3, 3, 1.0f, 0.0f, true);
    CHECK(next && next->executes == 2 && std::fabs(next->executeMs - 3.0f) < 1e-4f);
    CHECK(!pair.Add(1, 3, 9.0f, 9.0f, true));
    CHECK(!pair.Add(2, 3, 9.0f, 9.0f, true));
    CHECK(!pair.Add(1, 3, 1.0f, 0.0f, true));
    CHECK(!pair.Add(2, 3, 1.0f, 0.0f, true));
    const auto restarted = pair.Add(3, 3, 1.0f, 0.0f, true);
    CHECK(restarted && restarted->executes == 3 && std::fabs(restarted->executeMs - 3.0f) < 1e-4f);
    CHECK(!pair.Add(2, 3, 1.0f, 0.0f, true));
    CHECK(!pair.Add(3, 3, 1.0f, 0.0f, true));
    const auto single = pair.Add(1, 1, 2.0f, 1.0f, true);
    CHECK(single && single->executes == 1);
    CHECK(!pair.Add(0, 3, 1.0f, 1.0f, true) && !pair.Add(4, 3, 1.0f, 1.0f, true));
  }

  {
    const window_stats::Spread execute{300, 3.5f, 4.25f, 5.f}, copies{300, 1.f, 1.5f, 2.f}, total{300, 4.5f, 5.75f, 7.f};
    const std::string text = GpuCostText(execute, copies, total);
    CHECK(text == "gpu per real frame, median/p90 ms over 300 frames: execute 3.50/4.25 copies 1.00/1.50 total 4.50/5.75");
    CHECK(GpuCostText({}, {}, {}) == "gpu per real frame: none timed");
  }

  {
    int device = 0, other = 0;
    const ContextShape base{&device, 3840, 2160, 24, 1};
    ContextCache<int> cache;
    CHECK(cache.Find(base) == nullptr && cache.Size() == 0);
    cache.Add({base, 7, 5, 5});
    cache.Add({{&device, 2146, 1075, 24, 1}, 8, 1, 1});
    const auto* found = cache.Find(base);
    CHECK(found && found->handle == 7 && found->frames == 5 && found->serves == 5 && found->shape == base);
    const auto* small = cache.Find({&device, 2146, 1075, 24, 1});
    CHECK(small && small->handle == 8 && small->frames == 1);
    CHECK(cache.Size() == 2);
    CHECK(cache.Find({&other, 3840, 2160, 24, 1}) == nullptr);
    CHECK(cache.Find({&device, 3841, 2160, 24, 1}) == nullptr);
    CHECK(cache.Find({&device, 3840, 2161, 24, 1}) == nullptr);
    CHECK(cache.Find({&device, 3840, 2160, 25, 1}) == nullptr);
    CHECK(cache.Find({&device, 3840, 2160, 24, 0}) == nullptr);
    CHECK(cache.Find({&device, 3840, 2160, 24, 17}) == nullptr);
  }

  CHECK(FramesFor(3, 5) == 3);
  CHECK(FramesFor(5, 5) == 5);
  CHECK(FramesFor(6, 5) == 5);
  CHECK(FramesFor(0, 5) == 2);
  CHECK(FramesFor(1, 5) == 2);
  CHECK(FramesFor(2, 5) == 2);
  CHECK(FramesFor(1, 1) == 1);
  CHECK(FramesFor(3, 0) == 1);

  {
    int device = 0, other = 0;
    const ContextShape base{&device, 3840, 2160, 24, 1};
    const ContextShape small{&device, 2146, 1075, 24, 1};
    ContextCache<int> cache;
    cache.Add({base, 1, 3, 3});
    const std::optional<CurrentContext> current = CurrentContext{base, 3};
    const std::optional<CurrentContext> none;

    CHECK(ChooseContext(cache, current, base, 3).action == ContextAction::eKeep);
    CHECK(ChooseContext(cache, current, base, 2).action == ContextAction::eKeep);
    CHECK(ChooseContext(cache, current, base, 1).action == ContextAction::eKeep);
    CHECK(ChooseContext(cache, current, base, 4).action == ContextAction::eCreate);
    CHECK(ChooseContext(cache, none, base, 3).action == ContextAction::eSwitch);
    CHECK(ChooseContext(cache, none, base, 2).action == ContextAction::eSwitch);
    CHECK(ChooseContext(cache, none, base, 4).action == ContextAction::eCreate);
    CHECK(ChooseContext(cache, current, small, 1).action == ContextAction::eCreate);
    CHECK(ChooseContext(cache, none, {&other, 3840, 2160, 24, 1}, 1).action == ContextAction::eCreate);
    CHECK(ChooseContext(cache, std::optional<CurrentContext>(CurrentContext{small, 5}), base, 2).action
          == ContextAction::eSwitch);

    cache.Add({base, 2, 5, 5});
    cache.Add({base, 3, 4, 4});
    const auto tightest = ChooseContext(cache, none, base, 4);
    CHECK(tightest.action == ContextAction::eSwitch && tightest.cached->handle == 3);
    const auto smallest = ChooseContext(cache, none, base, 2);
    CHECK(smallest.action == ContextAction::eSwitch && smallest.cached->handle == 1);
    const auto largest = ChooseContext(cache, none, base, 5);
    CHECK(largest.action == ContextAction::eSwitch && largest.cached->handle == 2);
    const auto grown = ChooseContext(cache, current, base, 4);
    CHECK(grown.action == ContextAction::eSwitch && grown.cached->handle == 3);
    const auto preferred = ChooseContext(cache, std::optional<CurrentContext>(CurrentContext{base, 5}), base, 2);
    CHECK(preferred.action == ContextAction::eKeep);

    ContextCache<int> fallback;
    fallback.Add({base, 9, 1, 5});
    const auto served = ChooseContext(fallback, none, base, 3);
    CHECK(served.action == ContextAction::eSwitch && served.cached->handle == 9 && served.cached->frames == 1);
    CHECK(ChooseContext(fallback, std::optional<CurrentContext>(CurrentContext{base, 5}), base, 5).action
          == ContextAction::eKeep);
  }

  {
    int device = 0, other = 0;
    const ContextShape base{&device, 3840, 2160, 24, 1};
    const ContextShape second{&device, 2146, 1075, 24, 1};
    RefusedShapes refused;
    CHECK(refused.Find(base) == nullptr && refused.Size() == 0);
    refused.Add(base, "first");
    refused.Add(second, "second");
    CHECK(refused.Size() == 2);
    const auto* firstFound = refused.Find(base);
    CHECK(firstFound && firstFound->reason == "first");
    const auto* secondFound = refused.Find(second);
    CHECK(secondFound && secondFound->reason == "second");
    refused.Add(base, "again");
    CHECK(refused.Size() == 2 && refused.Find(base)->reason == "first");
    CHECK(refused.Find({&other, 3840, 2160, 24, 1}) == nullptr);
    CHECK(refused.Find({&device, 3841, 2160, 24, 1}) == nullptr);
    CHECK(refused.Find({&device, 3840, 2161, 24, 1}) == nullptr);
    CHECK(refused.Find({&device, 3840, 2160, 25, 1}) == nullptr);
    CHECK(refused.Find({&device, 3840, 2160, 24, 0}) == nullptr);
  }

  {
    int device = 0;
    const ContextShape second{&device, 2146, 1075, 24, 1}, third{&device, 1920, 1080, 24, 1};
    SettleClock clock;
    CHECK(!clock.Settled(second, 5000));
    CHECK(!clock.Settled(second, 5000 + kSettleMs - 1));
    CHECK(clock.Settled(second, 5000 + kSettleMs));
    CHECK(clock.Settled(second, 5000 + kSettleMs + 300));

    SettleClock interrupted;
    CHECK(!interrupted.Settled(second, 100));
    CHECK(!interrupted.Settled(second, 900));
    CHECK(!interrupted.Settled(third, 950));
    CHECK(!interrupted.Settled(second, 1000));
    CHECK(!interrupted.Settled(second, 1000 + kSettleMs - 1));
    CHECK(interrupted.Settled(second, 1000 + kSettleMs));

    SettleClock thirdWaits;
    CHECK(!thirdWaits.Settled(second, 0));
    CHECK(!thirdWaits.Settled(third, 600));
    CHECK(!thirdWaits.Settled(third, 600 + kSettleMs - 1));
    CHECK(thirdWaits.Settled(third, 600 + kSettleMs));

    SettleClock cancelled;
    CHECK(!cancelled.Settled(second, 0));
    CHECK(!cancelled.Settled(second, 800));
    cancelled.Clear();
    CHECK(!cancelled.Settled(second, 1200));
    CHECK(!cancelled.Settled(second, 1200 + kSettleMs - 1));
    CHECK(cancelled.Settled(second, 1200 + kSettleMs));
  }

  for (int i = 1; i < int(Idle::Count); ++i) CHECK(IdleText(Idle(i))[0] != '\0');
  CHECK(IdleText(Idle::None)[0] == '\0');
  CHECK(std::string(IdleText(Idle::OutputSettling)) == "output size settling");
  if (gFailures != 0) return 1;
  std::cout << "PASS: XMX decisions\n";
  return 0;
}
