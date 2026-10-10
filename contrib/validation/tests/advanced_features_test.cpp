// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>

#include "../../../src/addons/mfgunlock/advanced_features.hpp"

namespace {
unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (condition) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}

mfgunlock::advanced::Inputs Good(const mfgunlock::ArchitectureProfile* architecture) {
  namespace advanced = mfgunlock::advanced;
  namespace integration = mfgunlock::integration;
  advanced::Inputs input;
  input.architecture = architecture;
  input.provider_profile = &mfgunlock::profiles::kDlssg31091;
  input.provider_profile_index = 1;
  input.provider_exact = true;
  input.provider_ready = true;
  input.applied.full_temporal = true;
  input.applied.midpoint = false;
  input.applied.validated_warp = true;
  input.applied.scatter_retention = true;
  input.applied.boundary_mitigation = true;
  input.contract.frame_index = integration::Verdict::kValid;
  input.contract.constants_frame_known = 1;
  input.contract.constants_frame = 77;
  input.contract.resources = integration::Verdict::kValid;
  input.contract.history_reset_known = 1;
  input.contract.history_reset = 0;
  input.contract.rendering_game_frames_known = 1;
  input.contract.rendering_game_frames = 1;
  auto& depth = input.contract.resource[static_cast<size_t>(integration::ResourceRole::kDepth)];
  depth.seen = 1;
  depth.active = 1;
  depth.object = 0x100;
  depth.extent_known = 1;
  depth.extent_width = 1920;
  depth.extent_height = 1080;
  depth.frame_known = 1;
  depth.frame = 77;
  depth.provenance = integration::ResourceProvenance::kStreamline;
  auto& motion = input.contract.resource[
      static_cast<size_t>(integration::ResourceRole::kMotionVectors)];
  motion.seen = 1;
  motion.active = 1;
  motion.object = 0x200;
  motion.extent_known = 1;
  motion.extent_width = 1920;
  motion.extent_height = 1080;
  motion.frame_known = 1;
  motion.frame = 77;
  motion.provenance = integration::ResourceProvenance::kStreamline;
  return input;
}

const mfgunlock::advanced::FeatureState& State(
    const mfgunlock::advanced::Snapshot& snapshot,
    mfgunlock::advanced::Feature feature) {
  return snapshot.features[static_cast<size_t>(feature)];
}
}  // namespace

int main() {
  namespace advanced = mfgunlock::advanced;
  namespace integration = mfgunlock::integration;

  for (const auto* architecture : {&mfgunlock::architecture::kAda,
                                   &mfgunlock::architecture::kAmpere,
                                   &mfgunlock::architecture::kTuring}) {
    const auto snapshot = advanced::Evaluate(Good(architecture));
    Check(State(snapshot, advanced::Feature::kMotionValidation).backend ==
              advanced::Backend::kSoftware &&
          State(snapshot, advanced::Feature::kMotionValidation).input_eligible,
          "stable motion selects the validated software warp backend");
    Check(State(snapshot, advanced::Feature::kHistoryValidation).backend ==
              advanced::Backend::kSoftware &&
          State(snapshot, advanced::Feature::kHistoryValidation).input_eligible &&
          State(snapshot, advanced::Feature::kHistoryValidation).partial,
          "history validation is explicit architecture-independent partial software evidence");
    Check(State(snapshot, advanced::Feature::kOcclusionDisocclusion).backend ==
              advanced::Backend::kRetargeted &&
          State(snapshot, advanced::Feature::kOcclusionDisocclusion).input_eligible,
          "qualified inpaint pair exposes retargeted occlusion/disocclusion backend");
    Check(State(snapshot, advanced::Feature::kThinGeometryStability).backend ==
              advanced::Backend::kSoftware &&
          State(snapshot, advanced::Feature::kThinGeometryStability).input_eligible,
          "ISR exposes partial software thin-geometry stability");
    Check(State(snapshot, advanced::Feature::kBoundaryHandling).backend ==
              advanced::Backend::kSoftware &&
          State(snapshot, advanced::Feature::kBoundaryHandling).input_eligible,
          "boundary mitigation is a separate software backend");
    Check(State(snapshot, advanced::Feature::kScatterHistoryRetention).backend ==
              advanced::Backend::kSoftware &&
          State(snapshot, advanced::Feature::kScatterHistoryRetention).input_eligible,
          "scatter retention requires the full temporal backend and ISR");
    Check(State(snapshot, advanced::Feature::kConfidencePropagation).backend ==
              advanced::Backend::kUnavailable &&
          State(snapshot, advanced::Feature::kConfidencePropagation).fallback_reason ==
              advanced::FallbackReason::kNoVerifiedBackend,
          "confidence propagation remains unavailable without a verified contract");
    Check(State(snapshot, advanced::Feature::kFallbackSelection).backend ==
              advanced::Backend::kSoftware &&
          State(snapshot, advanced::Feature::kFallbackSelection).input_eligible,
          "fallback selection remains separate architecture-independent software logic");
  }

  auto input = Good(&mfgunlock::architecture::kAmpere);
  input.provider_exact = false;
  input.provider_profile = nullptr;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).input_eligible,
        "actual applied backend and input proofs are not vetoed by missing registry metadata");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.provider_ready = false;
  Check(State(advanced::Evaluate(input), advanced::Feature::kOcclusionDisocclusion).fallback_reason ==
            advanced::FallbackReason::kProviderNotReady,
        "unprepared retarget provider fails closed");

  input = Good(nullptr);
  Check(State(advanced::Evaluate(input), advanced::Feature::kFallbackSelection).fallback_reason ==
            advanced::FallbackReason::kNoArchitecture,
        "unknown architecture fails closed");

  input = Good(&mfgunlock::architecture::kAmpere);
  auto no_kernels = mfgunlock::profiles::kDlssg31091;
  no_kernels.quality_kernels = {};
  input.provider_profile = &no_kernels;
  Check(State(advanced::Evaluate(input), advanced::Feature::kOcclusionDisocclusion).input_eligible,
        "actual inpaint commit takes precedence over absent profile hints");
  input.applied.full_temporal = false;
  Check(State(advanced::Evaluate(input), advanced::Feature::kOcclusionDisocclusion).fallback_reason ==
            advanced::FallbackReason::kRequiredKernelMissing,
        "no actual kernel or profile evidence cannot claim an inpaint backend");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.resource[static_cast<size_t>(integration::ResourceRole::kDepth)].active = 0;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).fallback_reason ==
            advanced::FallbackReason::kDepthMissing,
        "missing depth tag blocks motion-dependent quality");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.resource[static_cast<size_t>(integration::ResourceRole::kMotionVectors)].active = 0;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).fallback_reason ==
            advanced::FallbackReason::kMotionVectorsMissing,
        "missing motion-vector tag blocks motion validation");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.resource[static_cast<size_t>(integration::ResourceRole::kMotionVectors)]
      .frame_known = 0;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).fallback_reason ==
            advanced::FallbackReason::kResourceFrameUnknown,
        "missing motion-frame association remains unknown instead of being guessed current");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.resource[static_cast<size_t>(integration::ResourceRole::kMotionVectors)].frame = 76;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).fallback_reason ==
            advanced::FallbackReason::kResourceFrameMismatch,
        "invalid motion-frame association fails closed");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.history_reset_known = 0;
  Check(State(advanced::Evaluate(input), advanced::Feature::kHistoryValidation).fallback_reason ==
            advanced::FallbackReason::kHistoryEvidenceMissing,
        "missing history evidence stays inactive");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.history_reset = 1;
  Check(State(advanced::Evaluate(input), advanced::Feature::kOcclusionDisocclusion).fallback_reason ==
            advanced::FallbackReason::kHistoryReset,
        "reset frame rejects temporal history");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.rendering_game_frames = 0;
  Check(State(advanced::Evaluate(input), advanced::Feature::kThinGeometryStability).fallback_reason ==
            advanced::FallbackReason::kNotRenderingGameFrames,
        "menu or cutscene frames do not claim advanced temporal quality");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.contract.frame_index = integration::Verdict::kInvalid;
  Check(State(advanced::Evaluate(input), advanced::Feature::kBoundaryHandling).fallback_reason ==
            advanced::FallbackReason::kFrameIdentityInvalid,
        "broken frame identity invalidates history-backed feature evidence");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.applied.full_temporal = false;
  Check(State(advanced::Evaluate(input), advanced::Feature::kOcclusionDisocclusion).fallback_reason ==
            advanced::FallbackReason::kTemporalBackendMissing,
        "occlusion/disocclusion does not fall back to midpoint semantics");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.applied.validated_warp = false;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).fallback_reason ==
            advanced::FallbackReason::kWarpBackendMissing,
        "motion validation requires the explicit validated-warp backend");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.applied.scatter_retention = false;
  Check(State(advanced::Evaluate(input), advanced::Feature::kThinGeometryStability).fallback_reason ==
            advanced::FallbackReason::kScatterBackendMissing,
        "thin geometry stability fails closed without ISR");

  input = Good(&mfgunlock::architecture::kAmpere);
  input.applied.boundary_mitigation = false;
  Check(State(advanced::Evaluate(input), advanced::Feature::kBoundaryHandling).fallback_reason ==
            advanced::FallbackReason::kBoundaryBackendMissing,
        "edge crossing remains partial without boundary mitigation");

  input = Good(&mfgunlock::architecture::kTuring);
  input.applied.full_temporal = false;
  input.applied.midpoint = true;
  const auto midpoint = advanced::Evaluate(input);
  Check(State(midpoint, advanced::Feature::kFallbackSelection).input_eligible &&
            State(midpoint, advanced::Feature::kFallbackSelection).backend ==
                advanced::Backend::kSoftware,
        "Turing midpoint remains an explicit software fallback");
  Check(!State(midpoint, advanced::Feature::kOcclusionDisocclusion).input_eligible,
        "midpoint fallback is not mislabeled as restored inpaint behavior");

  input = Good(&mfgunlock::architecture::kAda);
  auto& depth = input.contract.resource[static_cast<size_t>(integration::ResourceRole::kDepth)];
  depth.replacements = 1;
  depth.object = 0x3000;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).input_eligible,
        "resource replacement remains usable after current evidence is refreshed");

  input = Good(&mfgunlock::architecture::kAda);
  input.contract.history_reset_known = 0;
  auto evidence = advanced::Evaluate(input);
  const auto& installed = State(evidence, advanced::Feature::kOcclusionDisocclusion);
  Check(installed.backend == advanced::Backend::kRetargeted && !installed.input_eligible &&
        !installed.execution_known, "installed backend is not execution or eligible-input evidence");
  Check(State(evidence, advanced::Feature::kConfidencePropagation).category ==
        advanced::Category::kUnverified, "missing confidence contract is not hardware impossibility");
  input = Good(&mfgunlock::architecture::kAda);
  input.contract.resource[static_cast<size_t>(integration::ResourceRole::kDepth)].object = 0;
  Check(State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).fallback_reason ==
        advanced::FallbackReason::kResourceInvalid, "stale zero-identity resource is not usable");
  input = Good(&mfgunlock::architecture::kAda);
  input.contract.viewport_ownership = integration::Verdict::kInvalid;
  Check(!State(advanced::Evaluate(input), advanced::Feature::kMotionValidation).input_eligible,
        "ambiguous viewport cannot qualify advanced inputs");

  std::cout << "advanced_features_test: " << g_checks << " checks PASS\n";
  return 0;
}
