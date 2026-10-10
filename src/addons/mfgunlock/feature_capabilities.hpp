/*
 * Feature-capability metadata kept separate from provider preparation.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

#include "./architecture.hpp"

namespace mfgunlock::features {

// These bits describe target parity and architecture-specific backend availability only.
// Renderer, provider fingerprint, ABI and runtime capability gates stay in their owning layers.
enum class Feature : uint32_t {
  kFixedMfg = 1u << 0,
  kDynamicMfg = 1u << 1,
  kAutoMfg = 1u << 2,
  kTemporalFramework = 1u << 3,
  kIntermediateScatter = 1u << 4,
  kBoundaryMitigation = 1u << 5,
  kValidatedWarp = 1u << 6,
  kUiRecomposition = 1u << 7,
  kHdrPaths = 1u << 8,
  kVsyncSupport = 1u << 9,
  kDynamicResolution = 1u << 10,
  kQueueParallelism = 1u << 11,
  kMotionValidation = 1u << 12,
  kHistoryValidation = 1u << 13,
  kOcclusionDisocclusion = 1u << 14,
  kThinGeometryStability = 1u << 15,
  kConfidencePropagation = 1u << 16,
  kFallbackSelection = 1u << 17,
};

struct FeatureCapabilities {
  uint32_t bits = 0;

  constexpr bool Supports(Feature feature) const {
    return (bits & static_cast<uint32_t>(feature)) != 0;
  }
};

inline constexpr uint32_t kAllFeatureBits =
    static_cast<uint32_t>(Feature::kFixedMfg) |
    static_cast<uint32_t>(Feature::kDynamicMfg) |
    static_cast<uint32_t>(Feature::kAutoMfg) |
    static_cast<uint32_t>(Feature::kTemporalFramework) |
    static_cast<uint32_t>(Feature::kIntermediateScatter) |
    static_cast<uint32_t>(Feature::kBoundaryMitigation) |
    static_cast<uint32_t>(Feature::kValidatedWarp) |
    static_cast<uint32_t>(Feature::kUiRecomposition) |
    static_cast<uint32_t>(Feature::kHdrPaths) |
    static_cast<uint32_t>(Feature::kVsyncSupport) |
    static_cast<uint32_t>(Feature::kDynamicResolution) |
    static_cast<uint32_t>(Feature::kQueueParallelism) |
    static_cast<uint32_t>(Feature::kMotionValidation) |
    static_cast<uint32_t>(Feature::kHistoryValidation) |
    static_cast<uint32_t>(Feature::kOcclusionDisocclusion) |
    static_cast<uint32_t>(Feature::kThinGeometryStability) |
    static_cast<uint32_t>(Feature::kConfidencePropagation) |
    static_cast<uint32_t>(Feature::kFallbackSelection);

inline constexpr uint32_t kNativeSm89BackendBits =
    static_cast<uint32_t>(Feature::kFixedMfg) |
    static_cast<uint32_t>(Feature::kDynamicMfg) |
    static_cast<uint32_t>(Feature::kAutoMfg) |
    static_cast<uint32_t>(Feature::kTemporalFramework) |
    static_cast<uint32_t>(Feature::kIntermediateScatter) |
    static_cast<uint32_t>(Feature::kBoundaryMitigation) |
    static_cast<uint32_t>(Feature::kValidatedWarp) |
    static_cast<uint32_t>(Feature::kUiRecomposition) |
    static_cast<uint32_t>(Feature::kHdrPaths) |
    static_cast<uint32_t>(Feature::kVsyncSupport) |
    static_cast<uint32_t>(Feature::kDynamicResolution) |
    static_cast<uint32_t>(Feature::kQueueParallelism) |
    static_cast<uint32_t>(Feature::kMotionValidation) |
    static_cast<uint32_t>(Feature::kHistoryValidation) |
    static_cast<uint32_t>(Feature::kOcclusionDisocclusion) |
    static_cast<uint32_t>(Feature::kThinGeometryStability) |
    static_cast<uint32_t>(Feature::kFallbackSelection);

// Keep retargeted architecture capability declarations separate even while the
// currently verified feature sets are equal. Future evidence for one target
// must not silently enable the other.
inline constexpr uint32_t kRetargetSm86BackendBits = kNativeSm89BackendBits;
inline constexpr uint32_t kRetargetSm75BackendBits = kNativeSm89BackendBits;

inline constexpr FeatureCapabilities kTargetCapabilities{kAllFeatureBits};
inline constexpr FeatureCapabilities kNativeSm89BackendCapabilities{kNativeSm89BackendBits};
inline constexpr FeatureCapabilities kRetargetSm86BackendCapabilities{kRetargetSm86BackendBits};
inline constexpr FeatureCapabilities kRetargetSm75BackendCapabilities{kRetargetSm75BackendBits};
inline constexpr FeatureCapabilities kNoCapabilities{};

inline constexpr FeatureCapabilities TargetCapabilities(
    const ArchitectureProfile* profile) {
  return profile ? kTargetCapabilities : kNoCapabilities;
}

inline constexpr FeatureCapabilities BackendCapabilities(
    const ArchitectureProfile* profile) {
  if (!profile) return kNoCapabilities;
  switch (profile->provider_backend) {
    case ProviderBackend::kNativeSm89:
      return kNativeSm89BackendCapabilities;
    case ProviderBackend::kRetargetSm86:
      return kRetargetSm86BackendCapabilities;
    case ProviderBackend::kRetargetSm75:
      return kRetargetSm75BackendCapabilities;
    default:
      return kNoCapabilities;
  }
}

inline constexpr bool Targets(const ArchitectureProfile* profile, Feature feature) {
  return TargetCapabilities(profile).Supports(feature);
}

inline constexpr bool BackendSupports(const ArchitectureProfile* profile, Feature feature) {
  return BackendCapabilities(profile).Supports(feature);
}

inline constexpr bool HasQualityBackend(const ArchitectureProfile* profile) {
  return profile && BackendSupports(profile, Feature::kTemporalFramework) &&
         BackendSupports(profile, Feature::kIntermediateScatter) &&
         BackendSupports(profile, Feature::kBoundaryMitigation) &&
         BackendSupports(profile, Feature::kValidatedWarp);
}

inline constexpr bool HasRetargetedQualityBackend(const ArchitectureProfile* profile) {
  return profile && profile->RequiresProviderRetarget() && HasQualityBackend(profile);
}

}  // namespace mfgunlock::features
