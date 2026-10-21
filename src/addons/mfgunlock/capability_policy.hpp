/*
 * Architecture capability policy for native DLSS-G.
 * SPDX-License-Identifier: MIT
 *
 * Side-effect-free rules shared by the NGX/NVAPI hooks and portable tests.
 */
#pragma once

#include <climits>
#include <cstdint>

#include "./architecture.hpp"

namespace mfgunlock::policy {

using architecture::kNvidiaVendorId;
using architecture::kTuringArchitecture;
using architecture::kAmpereArchitecture;
using architecture::kAdaArchitecture;
inline constexpr uint32_t kDlssGFeatureId = 11;
inline constexpr uint32_t kNgxSuccess = 1;
inline constexpr uint32_t kAdapterUnsupported = 4;
inline constexpr uint32_t kStreamlineNoSupportedAdapter = 6;
inline constexpr uint32_t kStreamlineAdapterNotSupported = 7;

struct RequirementsEvidence {
  bool enabled;
  bool adapter_bound;
  unsigned int prepared_providers;
  uint32_t call_result;
  uint32_t feature;
  uint32_t flags;
  uint32_t minimum_architecture;
};

inline bool CanRelaxRequirements(const RequirementsEvidence& evidence,
                                 const ArchitectureProfile* profile) {
  // This adjusts a successful, adapter-scoped requirements query so the game
  // can attempt native creation. Preparation count and the advertised minimum
  // are not compatibility proofs: a new provider need not use Ada's threshold.
  // A profile is required only because the caller writes its actual native
  // architecture. Keep unrelated native errors/flags rather than inventing
  // their meaning or returning a false successful operation.
  return profile && evidence.enabled && evidence.adapter_bound &&
         evidence.call_result == kNgxSuccess &&
         evidence.feature == kDlssGFeatureId &&
         (evidence.flags == 0 || evidence.flags == kAdapterUnsupported) &&
         (evidence.flags == kAdapterUnsupported ||
          evidence.minimum_architecture > profile->native_arch);
}

inline bool CanExposeArchitecture(bool enabled, bool fg_requirements_scope, bool same_physical_gpu,
                                   bool provider_ready, int nvapi_result,
                                   const ArchitectureProfile* profile) {
  (void)provider_ready;  // Provider discovery must not prevent the scoped attempt.
  return profile && profile->RequiresProviderRetarget() && enabled && fg_requirements_scope &&
         same_physical_gpu && nvapi_result == 0;
}

struct StreamlineSupportEvidence {
  bool enabled;
  bool adapter_bound;
  bool adapter_matches;
  unsigned int prepared_providers;
  uint32_t result;
};

inline bool CanRelaxStreamlineSupport(const StreamlineSupportEvidence& evidence,
                                      const ArchitectureProfile* profile) {
  // Adapter admission is a prerequisite query, not an actual Create/Evaluate
  // result. Let the selected renderer attempt the native operation even when
  // its architecture is unknown or provider preparation was not observed.
  // Exact adapter ownership still prevents changing another GPU's answer.
  (void)profile;
  return evidence.enabled && evidence.adapter_bound && evidence.adapter_matches &&
         (evidence.result == kStreamlineNoSupportedAdapter ||
          evidence.result == kStreamlineAdapterNotSupported);
}

// Counts are generated frames, not presentation multipliers. The configured
// raise enables an empirical request; a parsed ceiling is diagnostic only.
// Never turn an older observation into a downward override of a native value.
inline int CapabilityFrameCount(int reported, unsigned int limit,
                                unsigned int structural_ceiling = 0) {
  (void)structural_ceiling;
  if (reported < 0 || limit == 0 || limit > static_cast<unsigned int>(INT_MAX)) return reported;
  return reported < static_cast<int>(limit) ? static_cast<int>(limit) : reported;
}


}  // namespace mfgunlock::policy
