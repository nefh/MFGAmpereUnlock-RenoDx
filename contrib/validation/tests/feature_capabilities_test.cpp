// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/feature_capabilities.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

using namespace mfgunlock;
namespace features = mfgunlock::features;

namespace {
unsigned int g_checks = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}
}  // namespace

int main() {
  constexpr std::array<features::Feature, 18> kFeatures = {
      features::Feature::kFixedMfg,
      features::Feature::kDynamicMfg,
      features::Feature::kAutoMfg,
      features::Feature::kTemporalFramework,
      features::Feature::kIntermediateScatter,
      features::Feature::kBoundaryMitigation,
      features::Feature::kValidatedWarp,
      features::Feature::kUiRecomposition,
      features::Feature::kHdrPaths,
      features::Feature::kVsyncSupport,
      features::Feature::kDynamicResolution,
      features::Feature::kQueueParallelism,
      features::Feature::kMotionValidation,
      features::Feature::kHistoryValidation,
      features::Feature::kOcclusionDisocclusion,
      features::Feature::kThinGeometryStability,
      features::Feature::kConfidencePropagation,
      features::Feature::kFallbackSelection,
  };

  for (const auto* profile : {&architecture::kAda, &architecture::kAmpere,
                              &architecture::kTuring}) {
    for (const auto feature : kFeatures) {
      Check(features::Targets(profile, feature),
            "known architectures share the same target feature set");
    }
  }

  Check(features::BackendSupports(&architecture::kAda, features::Feature::kFixedMfg),
        "Ada fixed MFG retained");
  Check(features::BackendSupports(&architecture::kAda, features::Feature::kDynamicMfg),
        "Ada Dynamic core restored on NativeSm89");
  Check(features::BackendSupports(&architecture::kAda, features::Feature::kTemporalFramework),
        "Ada native SM89 temporal framework restored");
  Check(features::BackendSupports(&architecture::kAda, features::Feature::kIntermediateScatter),
        "Ada native SM89 ISR restored");
  Check(features::BackendSupports(&architecture::kAda, features::Feature::kBoundaryMitigation),
        "Ada native SM89 Boundary restored");
  Check(features::BackendSupports(&architecture::kAda, features::Feature::kValidatedWarp),
        "Ada native SM89 Warp restored");
  Check(features::BackendSupports(&architecture::kAda, features::Feature::kUiRecomposition) &&
            features::BackendSupports(&architecture::kAda, features::Feature::kHdrPaths) &&
            features::BackendSupports(&architecture::kAda, features::Feature::kDynamicResolution) &&
            features::BackendSupports(&architecture::kAda, features::Feature::kQueueParallelism),
        "Ada shared integration paths remain available");

  constexpr std::array<features::Feature, 17> kVerifiedBackendFeatures = {
      features::Feature::kFixedMfg,
      features::Feature::kDynamicMfg,
      features::Feature::kAutoMfg,
      features::Feature::kTemporalFramework,
      features::Feature::kIntermediateScatter,
      features::Feature::kBoundaryMitigation,
      features::Feature::kValidatedWarp,
      features::Feature::kUiRecomposition,
      features::Feature::kHdrPaths,
      features::Feature::kVsyncSupport,
      features::Feature::kDynamicResolution,
      features::Feature::kQueueParallelism,
      features::Feature::kMotionValidation,
      features::Feature::kHistoryValidation,
      features::Feature::kOcclusionDisocclusion,
      features::Feature::kThinGeometryStability,
      features::Feature::kFallbackSelection,
  };

  for (const auto* profile : {&architecture::kAda, &architecture::kAmpere,
                              &architecture::kTuring}) {
    for (const auto feature : kVerifiedBackendFeatures) {
      Check(features::BackendSupports(profile, feature),
            "verified architecture backend capability remains explicit");
    }
    Check(!features::BackendSupports(profile, features::Feature::kConfidencePropagation),
          "confidence propagation stays unavailable without a verified backend");
  }

  for (const auto* profile : {&architecture::kAmpere, &architecture::kTuring}) {
    Check(features::HasRetargetedQualityBackend(profile),
          "retargeted quality backend remains available");
  }

  for (const auto feature : kFeatures) {
    Check(!features::Targets(nullptr, feature), "unresolved target capability fails closed");
    Check(!features::BackendSupports(nullptr, feature),
          "unresolved backend capability fails closed");
  }
  Check(features::HasQualityBackend(&architecture::kAda),
        "native SM89 has the shared quality feature set");
  Check(!features::HasRetargetedQualityBackend(&architecture::kAda),
        "native SM89 never enters retarget provider preparation");

  std::printf("PASS feature capabilities: %u checks\n", g_checks);
}
