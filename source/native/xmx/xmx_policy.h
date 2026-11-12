// SPDX-License-Identifier: MIT
#pragma once
#include <dxgiformat.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "window_stats.h"

namespace xmx::policy {
constexpr uint64_t kRetireAfterPairs = 16;
constexpr uint32_t kMaxContexts = 16;
constexpr uint64_t kStalePairMs = 500;
constexpr uint64_t kSettleMs = 1000;

template <class T>
class RetireQueue {
  struct Entry { T item; uint64_t due; };
  std::vector<Entry> entries;

 public:
  void Push(T item, uint64_t pair) { entries.push_back({std::move(item), pair + kRetireAfterPairs}); }
  void Collect(uint64_t pair) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.due <= pair; }),
                  entries.end());
  }
  size_t Size() const { return entries.size(); }
  template <class Visit>
  void ForEach(Visit&& visit) const {
    for (const auto& entry : entries) visit(entry.item);
  }
};

struct ContextShape {
  const void* device;
  uint32_t width, height, format, flags;
  friend constexpr bool operator==(const ContextShape&, const ContextShape&) = default;
};

constexpr uint32_t FramesFor(uint32_t requested, uint32_t modelMax) {
  const uint32_t wanted = requested > 2 ? requested : 2;
  const uint32_t limit = modelMax ? modelMax : 1;
  return wanted < limit ? wanted : limit;
}

template <class Handle>
class ContextCache {
 public:
  struct Entry {
    ContextShape shape;
    Handle handle;
    uint32_t frames;
    uint32_t serves;
  };

  const Entry* Find(const ContextShape& shape) const {
    for (const auto& entry : entries)
      if (entry.shape == shape) return &entry;
    return nullptr;
  }
  const Entry* FindServing(const ContextShape& shape, uint32_t frames) const {
    const Entry* best = nullptr;
    for (const auto& entry : entries)
      if (entry.shape == shape && entry.serves >= frames && (best == nullptr || entry.serves < best->serves))
        best = &entry;
    return best;
  }
  void Add(Entry entry) { entries.push_back(std::move(entry)); }
  size_t Size() const { return entries.size(); }

 private:
  std::vector<Entry> entries;
};

struct CurrentContext {
  ContextShape shape;
  uint32_t serves;
};

enum class ContextAction { eKeep, eSwitch, eCreate };

template <class Handle>
struct ContextChoice {
  ContextAction action;
  const typename ContextCache<Handle>::Entry* cached;
};

template <class Handle>
ContextChoice<Handle> ChooseContext(const ContextCache<Handle>& cache, const std::optional<CurrentContext>& current,
                                    const ContextShape& wanted, uint32_t frames) {
  if (current && current->shape == wanted && current->serves >= frames) return {ContextAction::eKeep, nullptr};
  if (const auto* cached = cache.FindServing(wanted, frames)) return {ContextAction::eSwitch, cached};
  return {ContextAction::eCreate, nullptr};
}

class RefusedShapes {
 public:
  struct Entry {
    ContextShape shape;
    std::string reason;
  };

  const Entry* Find(const ContextShape& shape) const {
    for (const auto& entry : entries)
      if (entry.shape == shape) return &entry;
    return nullptr;
  }
  void Add(const ContextShape& shape, std::string reason) {
    if (Find(shape) == nullptr) entries.push_back({shape, std::move(reason)});
  }
  size_t Size() const { return entries.size(); }

 private:
  std::vector<Entry> entries;
};

class SettleClock {
  ContextShape pending{};
  uint64_t since{};
  bool waiting{};

 public:
  bool Settled(const ContextShape& shape, uint64_t nowMs) {
    if (!waiting || !(pending == shape)) {
      pending = shape;
      since = nowMs;
      waiting = true;
    }
    return nowMs - since >= kSettleMs;
  }
  void Clear() { waiting = false; }
};

constexpr bool PairIsStale(uint64_t lastCaptureMs, uint64_t nowMs) {
  return lastCaptureMs != 0 && nowMs - lastCaptureMs > kStalePairMs;
}

struct FramePlan {
  uint32_t count, index;
  bool repeat, limited;
};
constexpr FramePlan PlanFrames(uint32_t requested, uint32_t index, uint32_t maxFrames) {
  const uint32_t wanted = requested ? requested : 1;
  const uint32_t count = wanted < maxFrames ? wanted : maxFrames;
  const uint32_t position = index < 1 ? 1 : index > wanted ? wanted : index;
  return {count, position < count ? position : count, position > count, wanted > count};
}

constexpr int CopyFamily(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
      return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 2;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return 3;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
      return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
      return 5;
    default:
      return 0;
  }
}
constexpr bool CanCopy(uint64_t targetWidth, uint32_t targetHeight, DXGI_FORMAT target, uint64_t sourceWidth,
                       uint32_t sourceHeight, DXGI_FORMAT source) {
  if (targetWidth != sourceWidth || targetHeight != sourceHeight) return false;
  return target == source || (CopyFamily(target) != 0 && CopyFamily(target) == CopyFamily(source));
}

enum class CopyMode { eWhole, eSubresourceZero };
constexpr CopyMode PickCopy(uint32_t targetMips, uint32_t targetSlices, uint32_t sourceMips, uint32_t sourceSlices) {
  const bool single = targetMips <= 1 && targetSlices <= 1 && sourceMips <= 1 && sourceSlices <= 1;
  return single ? CopyMode::eWhole : CopyMode::eSubresourceZero;
}

constexpr bool IsTypedDepth(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
         format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

constexpr bool Bypassed(bool hooked, bool everEvaluated, bool driverGenerating) {
  return hooked && !everEvaluated && driverGenerating;
}

struct Area {
  uint32_t x, y, width, height;
  friend constexpr bool operator==(const Area&, const Area&) = default;
};
constexpr Area InputArea(uint32_t textureWidth, uint32_t textureHeight, Area declared) {
  const bool fits = declared.width != 0 && declared.height != 0 &&
                    uint64_t{declared.x} + declared.width <= textureWidth &&
                    uint64_t{declared.y} + declared.height <= textureHeight;
  return fits ? declared : Area{0, 0, textureWidth, textureHeight};
}
constexpr std::pair<Area, Area> SharedArea(Area depth, Area motion) {
  const uint32_t width = depth.width < motion.width ? depth.width : motion.width;
  const uint32_t height = depth.height < motion.height ? depth.height : motion.height;
  return {Area{depth.x, depth.y, width, height}, Area{motion.x, motion.y, width, height}};
}

enum class Idle : int {
  None,
  MissingInputs,
  NoPair,
  GameReset,
  HudlessChanged,
  ExecuteError,
  NoOutput,
  CopyMismatch,
  DepthFormat,
  OutputSettling,
  Count
};

constexpr const char* IdleText(Idle idle) {
  switch (idle) {
    case Idle::MissingInputs: return "the game sent no color, depth or motion input";
    case Idle::NoPair: return "waiting for two real frames in a row";
    case Idle::GameReset: return "the game reset its history";
    case Idle::HudlessChanged: return "the game's HUD-less input came and went";
    case Idle::ExecuteError: return "XeFG returned an error";
    case Idle::NoOutput: return "XeFG produced no frame";
    case Idle::CopyMismatch: return "the game's textures do not match in size or format";
    case Idle::DepthFormat: return "the game's depth has a depth-stencil format XeFG's copy cannot be created in";
    case Idle::OutputSettling: return "output size settling";
    default: return "";
  }
}

enum Stamp : uint32_t { kStampStart, kStampRealCopied, kStampCaptured, kStampExecuted, kStampShown, kStampCount };
struct StampCost {
  float copyMs{}, executeMs{};
};
inline std::optional<StampCost> CostOfStamps(const std::array<uint64_t, kStampCount>& stamps, uint64_t frequency) {
  if (frequency == 0) return std::nullopt;
  for (size_t i = 0; i < stamps.size(); ++i)
    if (stamps[i] == 0 || (i > 0 && stamps[i] < stamps[i - 1])) return std::nullopt;
  const auto milliseconds = [&](Stamp from, Stamp to) {
    return static_cast<float>(static_cast<double>(stamps[to] - stamps[from]) * 1000.0 / static_cast<double>(frequency));
  };
  return StampCost{milliseconds(kStampStart, kStampCaptured) + milliseconds(kStampExecuted, kStampShown),
                   milliseconds(kStampCaptured, kStampExecuted)};
}

struct FrameCost {
  float executeMs{}, copyMs{};
  uint32_t executes{};
};
class PairCost {
  FrameCost total{};
  uint32_t seen{};

 public:
  std::optional<FrameCost> Add(uint32_t index, uint32_t requested, float executeMs, float copyMs, bool executed) {
    if (index == 0 || requested == 0 || requested > 30 || index > requested) return std::nullopt;
    if (index == 1) {
      total = {};
      seen = 0;
    }
    seen |= 1u << index;
    total.executeMs += executeMs;
    total.copyMs += copyMs;
    total.executes += executed ? 1 : 0;
    if (index < requested) return std::nullopt;
    const uint32_t wanted = (1u << (requested + 1)) - 2;
    const bool complete = seen == wanted;
    const FrameCost done = total;
    total = {};
    seen = 0;
    return complete ? std::optional<FrameCost>(done) : std::nullopt;
  }
};

inline std::string GpuCostText(const window_stats::Spread& execute, const window_stats::Spread& copies,
                               const window_stats::Spread& total) {
  if (total.events == 0) return "gpu per real frame: none timed";
  char text[200]{};
  std::snprintf(text, sizeof(text),
                "gpu per real frame, median/p90 ms over %llu frames: execute %.2f/%.2f copies %.2f/%.2f total %.2f/%.2f",
                static_cast<unsigned long long>(total.events), execute.median, execute.p90, copies.median, copies.p90,
                total.median, total.p90);
  return text;
}
}
