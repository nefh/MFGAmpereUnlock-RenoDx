// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>

namespace mfgunlock::qualityconfig {
inline constexpr uint32_t kUnknown = UINT32_MAX;
struct Config {
  bool temporal = true;
  bool scatter = false;
  uint32_t boundary = 0;
  bool warp = false;
  uint32_t warp_mode = 2;
};
inline uint32_t Encode(Config value) {
  return static_cast<uint32_t>(value.temporal) |
      (static_cast<uint32_t>(value.scatter) << 1) | ((value.boundary & 3) << 2) |
      (static_cast<uint32_t>(value.warp) << 4) | ((value.warp_mode & 3) << 5);
}
inline Config Decode(uint32_t value) {
  return {(value & 1) != 0, (value & 2) != 0, (value >> 2) & 3,
          (value & 16) != 0, (value >> 5) & 3};
}
inline uint32_t Effective(uint32_t value) {
  auto config = Decode(value);
  if (!config.temporal) config.scatter = false;
  if (!config.scatter) config.boundary = 0;
  if (!config.warp) config.warp_mode = 2;
  return Encode(config);
}
// Do not discard owners after failed restore: the descriptor may still refer
// to their replacement allocation, and a later teardown must retain its evidence.
template <typename Records, typename Restore>
inline bool RestoreRecords(Records& records, Restore restore) {
  for (auto it = records.begin(); it != records.end();) {
    if (restore(*it)) it = records.erase(it);
    else ++it;
  }
  return records.empty();
}
inline bool Pending(uint32_t requested, uint32_t prepared) {
  return prepared != kUnknown && Effective(requested) != Effective(prepared);
}
inline const char* ActionText(bool pending, bool used, bool recreated,
                              bool retained, bool uncertain) {
  if (!pending) return nullptr;
  if (uncertain)
    return "Quality changes pending. Feature tracking is incomplete; restart the game to apply.";
  if (retained)
    return "Quality changes pending. The game retained its DLSS-G feature; restart the game to apply.";
  if (recreated)
    return "Quality changes pending. FG was recreated, but kernel-cache reload is not verified; restart the game to apply.";
  if (used)
    return "Kernel quality changes pending. Safe CUDA kernel reload is not verified; restart the game to apply.";
  return "Quality changes pending. The provider is already prepared; restart the game to apply.";
}
inline const char* PresetAction(int requested, int supplied, int observed) {
  if (requested == 0) return supplied > 0
      ? "Preset override removed; the provider may retain its loaded model. Toggle FG to refresh it."
      : nullptr;
  if (requested == observed) return nullptr;
  return "Preset pending. Turn Frame Generation off and on in the game menu to apply.";
}
// Source fatbins are consumed during preparation, before NGX Create. A later
// Release/Create is not proof that the driver's loaded CUDA modules were retired.
inline std::atomic<uint32_t> g_requested{Encode(Config{})};
inline std::atomic<uint32_t> g_prepared{kUnknown};
inline uint32_t Freeze() {
  uint32_t expected = kUnknown;
  g_prepared.compare_exchange_strong(expected, g_requested.load(std::memory_order_acquire),
                                     std::memory_order_acq_rel);
  return g_prepared.load(std::memory_order_acquire);
}
inline Config Prepared() { return Decode(Freeze()); }
}  // namespace mfgunlock::qualityconfig
