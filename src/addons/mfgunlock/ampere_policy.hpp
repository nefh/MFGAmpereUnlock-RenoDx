/*
 * Architecture capability policy for native DLSS-G.
 * SPDX-License-Identifier: MIT
 *
 * Side-effect-free rules shared by the NGX/NVAPI hooks and portable tests.
 */
#pragma once

#include <cstdint>

#include "./architecture.hpp"

namespace mfgunlock::ampere {

using architecture::kNvidiaVendorId;
using architecture::kTuringArchitecture;
using architecture::kAmpereArchitecture;
using architecture::kAdaArchitecture;
inline constexpr uint32_t kDlssGFeatureId = 11;
inline constexpr uint32_t kNgxSuccess = 1;
inline constexpr uint32_t kAdapterUnsupported = 4;

struct RequirementsEvidence {
  bool enabled;
  bool adapter_bound;
  unsigned int prepared_providers;
  uint32_t call_result;
  uint32_t feature;
  uint32_t flags;
  uint32_t minimum_architecture;
};

inline const char* RequirementsDecision(const RequirementsEvidence& evidence,
                                         const ArchitectureProfile* profile) {
  if (!evidence.enabled) return "disabled";
  if (evidence.feature != kDlssGFeatureId) return "not-DLSS-G";
  if (evidence.call_result != kNgxSuccess) return "original-call-failed";
  if (!profile || !profile->NeedsRetarget()) return "no-backport";
  if (!evidence.adapter_bound) return "adapter-not-bound";
  if (evidence.prepared_providers != 1) return "provider-not-ready-or-ambiguous";
  if (evidence.flags != 0 && evidence.flags != kAdapterUnsupported)
    return "other-requirements-preserved";
  if (evidence.minimum_architecture != profile->native_arch &&
      evidence.minimum_architecture != profile->exposed_arch) {
    return "unknown-minimum-architecture";
  }
  if (evidence.flags == 0 && evidence.minimum_architecture == profile->native_arch)
    return "already-supported";
  return "architecture-override";
}

inline bool CanRelaxRequirements(const RequirementsEvidence& evidence,
                                 const ArchitectureProfile* profile) {
  // AdapterUnsupported also covers non-architecture failures. Preserve all
  // other requirement flags and only override for the prepared DLSS-G provider.
  return profile && profile->NeedsRetarget() && evidence.enabled && evidence.adapter_bound &&
         evidence.prepared_providers == 1 && evidence.call_result == kNgxSuccess &&
         evidence.feature == kDlssGFeatureId &&
         (evidence.flags == 0 || evidence.flags == kAdapterUnsupported) &&
         (evidence.minimum_architecture == profile->exposed_arch ||
          (evidence.minimum_architecture == profile->native_arch &&
           evidence.flags == kAdapterUnsupported));
}

inline bool CanExposeArchitecture(bool enabled, bool fg_requirements_scope, bool same_physical_gpu,
                                   bool provider_ready, int nvapi_result,
                                   const ArchitectureProfile* profile) {
  return profile && profile->NeedsRetarget() && enabled && fg_requirements_scope &&
         same_physical_gpu && provider_ready && nvapi_result == 0;
}

// These helpers decode samples returned successfully by Windows. They do not
// alter scheduling state or turn a failed query into a valid one.
inline bool Hags27Enabled(uint32_t value) {
  return (value & 3u) == 3u;
}

inline bool Hags29Enabled(uint32_t value) {
  return (value & 3u) != 0 && (value & 4u) != 0;
}

}  // namespace mfgunlock::ampere
