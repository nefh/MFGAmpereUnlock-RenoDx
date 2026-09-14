/*
 * Conservative UI candidate policy for Streamline UI recomposition.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace mfgunlock::uicandidate {

constexpr uint32_t kRequiredStableFrames = 6;
constexpr uint32_t kMinRtvBindsAfterClear = 4;
constexpr uint32_t kMaxRtvBindsAfterClear = 8;
constexpr uint32_t kHandoverWarmupFrames = 2;
constexpr uint64_t kHandoverMaxGapMs = 500;
constexpr uint32_t kRetryCooldownFrames = 3;
constexpr uint32_t kMaxConsecutiveInjectionFailures = 4;

enum RejectReason : uint32_t {
  kAccept = 0,
  kWrongOutput = 1u << 0,
  kSwapchain = 1u << 1,
  kAlreadyTagged = 1u << 2,
  kWrongFormat = 1u << 3,
  kNoFreshTransparentClear = 1u << 4,
  kUnexpectedPassCount = 1u << 5,
  kStateUnknown = 1u << 6,
  kStale = 1u << 7,
  kAmbiguous = 1u << 8,
  kLateWrite = 1u << 9,
};

struct Evidence {
  bool output_match = false;
  bool swapchain = false;
  bool already_tagged = false;
  bool rgba8_with_alpha = false;
  bool fresh_transparent_clear = false;
  bool state_known = false;
  bool recent = false;
  uint32_t rtv_binds_after_clear = 0;
  uint32_t late_writes = 0;
};

inline uint32_t Assess(const Evidence& evidence) {
  uint32_t reasons = kAccept;
  if (!evidence.output_match) reasons |= kWrongOutput;
  if (evidence.swapchain) reasons |= kSwapchain;
  if (evidence.already_tagged) reasons |= kAlreadyTagged;
  if (!evidence.rgba8_with_alpha) reasons |= kWrongFormat;
  if (!evidence.fresh_transparent_clear) reasons |= kNoFreshTransparentClear;
  if (evidence.rtv_binds_after_clear < kMinRtvBindsAfterClear ||
      evidence.rtv_binds_after_clear > kMaxRtvBindsAfterClear) {
    reasons |= kUnexpectedPassCount;
  }
  if (!evidence.state_known) reasons |= kStateUnknown;
  if (!evidence.recent) reasons |= kStale;
  if (evidence.late_writes != 0) reasons |= kLateWrite;
  return reasons;
}

inline bool MatchesSignature(const Evidence& evidence) {
  return Assess(evidence) == kAccept;
}

inline bool IsStable(uint32_t stable_frames) {
  return stable_frames >= kRequiredStableFrames;
}

inline bool CanWarmHandover(bool family_valid, bool signature_match,
                            uint64_t family_age_ms) {
  return family_valid && signature_match && family_age_ms <= kHandoverMaxGapMs;
}

inline uint32_t HandoverSeedStableFrames() {
  static_assert(kRequiredStableFrames >= kHandoverWarmupFrames);
  return kRequiredStableFrames - kHandoverWarmupFrames + 1;
}

inline uint32_t NextFailureStreak(bool same_resource, uint32_t previous_streak) {
  return same_resource ? previous_streak + 1 : 1;
}

inline bool ShouldHardDecline(uint32_t consecutive_failures) {
  return consecutive_failures >= kMaxConsecutiveInjectionFailures;
}

inline bool CanInject(uint32_t reject_reasons, uint32_t stable_frames,
                      bool command_buffer_available, bool options_synced,
                      bool injection_enabled) {
  return injection_enabled && command_buffer_available && options_synced &&
         IsStable(stable_frames) && reject_reasons == kAccept;
}

inline bool CanInject(const Evidence& evidence, uint32_t stable_frames,
                      bool command_buffer_available, bool options_synced,
                      bool injection_enabled) {
  return CanInject(Assess(evidence), stable_frames, command_buffer_available,
                   options_synced, injection_enabled);
}

}  // namespace mfgunlock::uicandidate
