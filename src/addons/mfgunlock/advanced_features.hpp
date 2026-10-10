// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "./architecture.hpp"
#include "./feature_capabilities.hpp"
#include "./integration_contract.hpp"
#include "./provider_profile.hpp"

namespace mfgunlock::advanced {

inline constexpr uint32_t kVersion = 2;

enum class Feature : uint32_t {
  kMotionValidation = 0,
  kHistoryValidation,
  kOcclusionDisocclusion,
  kThinGeometryStability,
  kBoundaryHandling,
  kScatterHistoryRetention,
  kConfidencePropagation,
  kFallbackSelection,
  kCount,
};

enum class Category : uint32_t {
  kArchitectureIndependent = 0,
  kRetargetableGpuCode,
  kHardwareSpecificReplaceable,
  kGenuinelyUnavailable,
  kUnverified,
};

enum class Backend : uint32_t {
  kUnavailable = 0,
  kNative,
  kRetargeted,
  kSoftware,
};

enum class FallbackReason : uint32_t {
  kNone = 0,
  kNoArchitecture,
  kProviderNotExact,
  kProviderNotReady,
  kProviderProfileMissing,
  kTargetUnsupported,
  kRequiredKernelMissing,
  kDepthMissing,
  kMotionVectorsMissing,
  kResourceFrameUnknown,
  kResourceFrameMismatch,
  kHistoryEvidenceMissing,
  kHistoryReset,
  kNotRenderingGameFrames,
  kFrameIdentityInvalid,
  kTemporalBackendMissing,
  kWarpBackendMissing,
  kScatterBackendMissing,
  kBoundaryBackendMissing,
  kNoVerifiedBackend,
  kFallbackUnavailable,
  kResourceInvalid,
  kViewportAmbiguous,
};

struct AppliedBackends {
  bool full_temporal = false;
  bool midpoint = false;
  bool validated_warp = false;
  bool scatter_retention = false;
  bool boundary_mitigation = false;
};

struct Inputs {
  const ArchitectureProfile* architecture = nullptr;
  const profiles::ProviderProfile* provider_profile = nullptr;
  uint32_t provider_profile_index = 0;
  bool provider_exact = false;
  bool provider_ready = false;
  AppliedBackends applied{};
  integration::Snapshot contract{};
};

struct FeatureState {
  Feature feature = Feature::kMotionValidation;
  Category category = Category::kArchitectureIndependent;
  Backend backend = Backend::kUnavailable;
  // Installed code and eligible inputs do not prove GPU execution or quality.
  uint32_t input_eligible = 0;
  uint32_t execution_known = 0;
  uint32_t execution_active = 0;
  uint32_t partial = 0;
  FallbackReason fallback_reason = FallbackReason::kNone;
};

struct Snapshot {
  uint32_t bytes = sizeof(Snapshot);
  uint32_t version = kVersion;
  uint32_t architecture = static_cast<uint32_t>(Architecture::kUnknown);
  uint32_t provider_backend = static_cast<uint32_t>(ProviderBackend::kUnknown);
  uint32_t target_sm = 0;
  uint32_t provider_profile_index = 0;
  uint32_t provider_exact = 0;
  uint32_t provider_ready = 0;
  uint32_t resource_contract = static_cast<uint32_t>(integration::Verdict::kUnknown);
  uint32_t history_reset_known = 0;
  uint32_t history_reset = 0;
  uint32_t rendering_game_frames_known = 0;
  uint32_t rendering_game_frames = 0;
  std::array<FeatureState, static_cast<size_t>(Feature::kCount)> features{};
};

inline constexpr features::Feature CapabilityFor(Feature feature) {
  switch (feature) {
    case Feature::kMotionValidation: return features::Feature::kMotionValidation;
    case Feature::kHistoryValidation: return features::Feature::kHistoryValidation;
    case Feature::kOcclusionDisocclusion: return features::Feature::kOcclusionDisocclusion;
    case Feature::kThinGeometryStability: return features::Feature::kThinGeometryStability;
    case Feature::kBoundaryHandling: return features::Feature::kBoundaryMitigation;
    case Feature::kScatterHistoryRetention: return features::Feature::kIntermediateScatter;
    case Feature::kConfidencePropagation: return features::Feature::kConfidencePropagation;
    case Feature::kFallbackSelection: return features::Feature::kFallbackSelection;
    default: return features::Feature::kConfidencePropagation;
  }
}

inline constexpr Category CategoryFor(Feature feature) {
  switch (feature) {
    case Feature::kHistoryValidation:
    case Feature::kFallbackSelection:
      return Category::kArchitectureIndependent;
    case Feature::kOcclusionDisocclusion:
      return Category::kRetargetableGpuCode;
    case Feature::kMotionValidation:
    case Feature::kThinGeometryStability:
    case Feature::kBoundaryHandling:
    case Feature::kScatterHistoryRetention:
      return Category::kHardwareSpecificReplaceable;
    case Feature::kConfidencePropagation:
    default:
      return Category::kUnverified;
  }
}

inline constexpr const char* FeatureName(Feature feature) {
  switch (feature) {
    case Feature::kMotionValidation: return "motion_validation";
    case Feature::kHistoryValidation: return "history_validation_rejection";
    case Feature::kOcclusionDisocclusion: return "occlusion_disocclusion";
    case Feature::kThinGeometryStability: return "thin_geometry_stability";
    case Feature::kBoundaryHandling: return "boundary_handling";
    case Feature::kScatterHistoryRetention: return "scatter_history_retention";
    case Feature::kConfidencePropagation: return "confidence_propagation";
    case Feature::kFallbackSelection: return "fallback_selection";
    default: return "unknown";
  }
}

inline constexpr const char* CategoryName(Category category) {
  switch (category) {
    case Category::kArchitectureIndependent: return "ARCHITECTURE_INDEPENDENT";
    case Category::kRetargetableGpuCode: return "RETARGETABLE_GPU_CODE";
    case Category::kHardwareSpecificReplaceable: return "HARDWARE_SPECIFIC_REPLACEABLE";
    case Category::kGenuinelyUnavailable: return "GENUINELY_UNAVAILABLE";
    case Category::kUnverified: return "UNVERIFIED";
  }
  return "UNKNOWN";
}

inline constexpr const char* BackendName(Backend backend) {
  switch (backend) {
    case Backend::kNative: return "Native";
    case Backend::kRetargeted: return "Retargeted";
    case Backend::kSoftware: return "Software";
    case Backend::kUnavailable: return "Unavailable";
  }
  return "Unavailable";
}

inline constexpr const char* ProviderBackendName(ProviderBackend backend) {
  switch (backend) {
    case ProviderBackend::kNativeSm89: return "NativeSm89";
    case ProviderBackend::kRetargetSm86: return "RetargetSm86";
    case ProviderBackend::kRetargetSm75: return "RetargetSm75";
    case ProviderBackend::kUnknown: return "Unknown";
  }
  return "Unknown";
}

inline constexpr const char* FallbackReasonName(FallbackReason reason) {
  switch (reason) {
    case FallbackReason::kNone: return "NONE";
    case FallbackReason::kResourceInvalid: return "RESOURCE_INVALID";
    case FallbackReason::kViewportAmbiguous: return "VIEWPORT_AMBIGUOUS";
    case FallbackReason::kNoArchitecture: return "NO_ARCHITECTURE";
    case FallbackReason::kProviderNotExact: return "PROVIDER_NOT_EXACT";
    case FallbackReason::kProviderNotReady: return "PROVIDER_NOT_READY";
    case FallbackReason::kProviderProfileMissing: return "PROVIDER_PROFILE_MISSING";
    case FallbackReason::kTargetUnsupported: return "TARGET_UNSUPPORTED";
    case FallbackReason::kRequiredKernelMissing: return "REQUIRED_KERNEL_MISSING";
    case FallbackReason::kDepthMissing: return "DEPTH_MISSING";
    case FallbackReason::kMotionVectorsMissing: return "MOTION_VECTORS_MISSING";
    case FallbackReason::kResourceFrameUnknown: return "RESOURCE_FRAME_UNKNOWN";
    case FallbackReason::kResourceFrameMismatch: return "RESOURCE_FRAME_MISMATCH";
    case FallbackReason::kHistoryEvidenceMissing: return "HISTORY_EVIDENCE_MISSING";
    case FallbackReason::kHistoryReset: return "HISTORY_RESET";
    case FallbackReason::kNotRenderingGameFrames: return "NOT_RENDERING_GAME_FRAMES";
    case FallbackReason::kFrameIdentityInvalid: return "FRAME_IDENTITY_INVALID";
    case FallbackReason::kTemporalBackendMissing: return "TEMPORAL_BACKEND_MISSING";
    case FallbackReason::kWarpBackendMissing: return "WARP_BACKEND_MISSING";
    case FallbackReason::kScatterBackendMissing: return "SCATTER_BACKEND_MISSING";
    case FallbackReason::kBoundaryBackendMissing: return "BOUNDARY_BACKEND_MISSING";
    case FallbackReason::kNoVerifiedBackend: return "NO_VERIFIED_BACKEND";
    case FallbackReason::kFallbackUnavailable: return "FALLBACK_UNAVAILABLE";
  }
  return "UNKNOWN";
}

inline const integration::ResourceState& Resource(
    const integration::Snapshot& contract, integration::ResourceRole role) {
  return contract.resource[static_cast<size_t>(role)];
}

inline FallbackReason ProviderGate(const Inputs& input, Feature feature) {
  if (!input.architecture) return FallbackReason::kNoArchitecture;
  if (!features::BackendSupports(input.architecture, CapabilityFor(feature)))
    return FallbackReason::kNoVerifiedBackend;
  // Individual applied-backend and resource proofs below determine features;
  // a private registry entry is not evidence that all (or none) can execute.
  if (!input.provider_ready) return FallbackReason::kProviderNotReady;
  return FallbackReason::kNone;
}

inline FallbackReason ResourceFrameGate(const integration::Snapshot& contract,
                                        integration::ResourceRole role) {
  const auto& resource = Resource(contract, role);
  if (!resource.active || resource.object == 0 || !resource.extent_known ||
      resource.extent_width == 0 || resource.extent_height == 0)
    return FallbackReason::kResourceInvalid;
  if (contract.viewport_ownership == integration::Verdict::kInvalid)
    return FallbackReason::kViewportAmbiguous;
  if (!contract.constants_frame_known || !resource.frame_known)
    return FallbackReason::kResourceFrameUnknown;
  return resource.frame == contract.constants_frame
      ? FallbackReason::kNone : FallbackReason::kResourceFrameMismatch;
}

inline FallbackReason MotionInputs(const integration::Snapshot& contract) {
  const auto& depth = Resource(contract, integration::ResourceRole::kDepth);
  if (!depth.active) return FallbackReason::kDepthMissing;
  const auto& motion = Resource(contract, integration::ResourceRole::kMotionVectors);
  if (!motion.active) return FallbackReason::kMotionVectorsMissing;
  const auto depth_frame = ResourceFrameGate(contract, integration::ResourceRole::kDepth);
  if (depth_frame != FallbackReason::kNone) return depth_frame;
  const auto motion_frame = ResourceFrameGate(
      contract, integration::ResourceRole::kMotionVectors);
  if (motion_frame != FallbackReason::kNone) return motion_frame;
  return FallbackReason::kNone;
}

inline FallbackReason HistoryGate(const integration::Snapshot& contract) {
  if (contract.frame_index == integration::Verdict::kInvalid)
    return FallbackReason::kFrameIdentityInvalid;
  if (!contract.constants_frame_known || !contract.history_reset_known ||
      !contract.rendering_game_frames_known)
    return FallbackReason::kHistoryEvidenceMissing;
  if (contract.history_reset) return FallbackReason::kHistoryReset;
  if (!contract.rendering_game_frames) return FallbackReason::kNotRenderingGameFrames;
  return FallbackReason::kNone;
}

inline FallbackReason CommonQualityGate(const Inputs& input, Feature feature) {
  const auto provider = ProviderGate(input, feature);
  if (provider != FallbackReason::kNone) return provider;
  const auto motion = MotionInputs(input.contract);
  if (motion != FallbackReason::kNone) return motion;
  return HistoryGate(input.contract);
}

inline bool HasKernel(const Inputs& input, std::string_view role) {
  // A recorded successful transform is direct kernel evidence, including on
  // images without a registry entry. Never replace it with a metadata veto.
  if (role == "warp" && input.applied.validated_warp) return true;
  if ((role == "inpaint" || role == "inpaint-decision") && input.applied.full_temporal) return true;
  if (role == "motion-vector" && (input.applied.full_temporal ||
      input.applied.scatter_retention || input.applied.boundary_mitigation)) return true;
  return input.provider_profile && profiles::HasQualityKernel(*input.provider_profile, role);
}

inline FeatureState SelectBackend(const Inputs& input, Feature feature) {
  FeatureState state;
  state.feature = feature;
  state.category = CategoryFor(feature);

  if (feature == Feature::kConfidencePropagation) {
    state.fallback_reason = FallbackReason::kNoVerifiedBackend;
    return state;
  }

  const auto provider = ProviderGate(input, feature);
  if (provider != FallbackReason::kNone) {
    state.fallback_reason = provider;
    return state;
  }

  if (feature == Feature::kHistoryValidation) {
    state.backend = Backend::kSoftware;
    state.partial = 1;
    state.fallback_reason = HistoryGate(input.contract);
    state.input_eligible = state.fallback_reason == FallbackReason::kNone;
    return state;
  }

  if (feature == Feature::kFallbackSelection) {
    state.backend = Backend::kSoftware;
    state.partial = 1;
    if (!input.applied.full_temporal && !input.applied.midpoint) {
      state.backend = Backend::kUnavailable;
      state.fallback_reason = FallbackReason::kFallbackUnavailable;
      return state;
    }
    state.input_eligible = 1;
    return state;
  }

  switch (feature) {
    case Feature::kMotionValidation:
      if (!HasKernel(input, "warp")) {
        state.fallback_reason = FallbackReason::kRequiredKernelMissing;
        return state;
      }
      state.backend = input.applied.validated_warp ? Backend::kSoftware : Backend::kUnavailable;
      state.partial = 1;
      state.input_eligible = input.applied.validated_warp;
      state.fallback_reason = state.input_eligible ? FallbackReason::kNone
                                           : FallbackReason::kWarpBackendMissing;
      return state;

    case Feature::kOcclusionDisocclusion:
      if (!HasKernel(input, "inpaint") || !HasKernel(input, "inpaint-decision")) {
        state.fallback_reason = FallbackReason::kRequiredKernelMissing;
        return state;
      }
      state.backend = input.applied.full_temporal ? Backend::kRetargeted
                                                  : Backend::kUnavailable;
      state.input_eligible = input.applied.full_temporal;
      state.fallback_reason = state.input_eligible ? FallbackReason::kNone
                                           : FallbackReason::kTemporalBackendMissing;
      return state;

    case Feature::kThinGeometryStability:
      if (!HasKernel(input, "motion-vector")) {
        state.fallback_reason = FallbackReason::kRequiredKernelMissing;
        return state;
      }
      state.backend = input.applied.scatter_retention ? Backend::kSoftware
                                                      : Backend::kUnavailable;
      state.partial = 1;
      state.input_eligible = input.applied.scatter_retention;
      state.fallback_reason = state.input_eligible ? FallbackReason::kNone
                                           : FallbackReason::kScatterBackendMissing;
      return state;

    case Feature::kBoundaryHandling:
      if (!HasKernel(input, "motion-vector")) {
        state.fallback_reason = FallbackReason::kRequiredKernelMissing;
        return state;
      }
      state.backend = input.applied.boundary_mitigation ? Backend::kSoftware
                                                        : Backend::kUnavailable;
      state.partial = 1;
      state.input_eligible = input.applied.boundary_mitigation;
      state.fallback_reason = state.input_eligible ? FallbackReason::kNone
                                           : FallbackReason::kBoundaryBackendMissing;
      return state;

    case Feature::kScatterHistoryRetention:
      if (!HasKernel(input, "motion-vector")) {
        state.fallback_reason = FallbackReason::kRequiredKernelMissing;
        return state;
      }
      state.backend = input.applied.scatter_retention ? Backend::kSoftware
                                                      : Backend::kUnavailable;
      state.partial = 1;
      state.input_eligible = input.applied.scatter_retention && input.applied.full_temporal;
      state.fallback_reason = state.input_eligible ? FallbackReason::kNone
          : input.applied.scatter_retention ? FallbackReason::kTemporalBackendMissing
                                            : FallbackReason::kScatterBackendMissing;
      return state;

    default:
      state.fallback_reason = FallbackReason::kNoVerifiedBackend;
      return state;
  }
}

inline FeatureState EvaluateFeature(const Inputs& input, Feature feature) {
  auto state = SelectBackend(input, feature);
  if (state.input_eligible && feature != Feature::kFallbackSelection &&
      feature != Feature::kHistoryValidation) {
    state.fallback_reason = CommonQualityGate(input, feature);
    state.input_eligible = state.fallback_reason == FallbackReason::kNone;
  }
  return state;
}

inline Snapshot Evaluate(const Inputs& input) {
  Snapshot out;
  if (input.architecture) {
    out.architecture = static_cast<uint32_t>(input.architecture->architecture);
    out.provider_backend = static_cast<uint32_t>(input.architecture->provider_backend);
    out.target_sm = input.architecture->target_sm;
  }
  out.provider_profile_index = input.provider_profile_index;
  out.provider_exact = input.provider_exact;
  out.provider_ready = input.provider_ready;
  out.resource_contract = static_cast<uint32_t>(input.contract.resources);
  out.history_reset_known = input.contract.history_reset_known;
  out.history_reset = input.contract.history_reset;
  out.rendering_game_frames_known = input.contract.rendering_game_frames_known;
  out.rendering_game_frames = input.contract.rendering_game_frames;
  for (size_t index = 0; index < out.features.size(); ++index)
    out.features[index] = EvaluateFeature(input, static_cast<Feature>(index));
  return out;
}

using Query = bool (*)(uint32_t, void*);

}  // namespace mfgunlock::advanced
