/*
 * Ampere capability policy for native DLSS-G.
 * SPDX-License-Identifier: MIT
 *
 * This file contains only side-effect-free admission rules. The runtime hooks
 * live in ampere_ngx.hpp; keeping the decisions here makes the fail-closed
 * behavior easy to test without loading NVAPI, NGX, or Streamline.
 */
#pragma once

#include <cstdint>

namespace mfgunlock::ampere {

inline constexpr uint32_t kNvidiaVendorId = 0x10de;
inline constexpr uint32_t kAmpereArchitecture = 0x170;
inline constexpr uint32_t kAdaArchitecture = 0x190;
inline constexpr uint32_t kDlssGFeatureId = 11;
inline constexpr uint32_t kNgxSuccess = 1;
inline constexpr uint32_t kAdapterUnsupported = 4;

struct RequirementsEvidence {
  bool enabled;
  bool ampere_adapter;
  unsigned int prepared_providers;
  uint32_t call_result;
  uint32_t feature;
  uint32_t flags;
  uint32_t minimum_architecture;
};

inline const char* RequirementsDecision(const RequirementsEvidence& evidence) {
  if (!evidence.enabled) return "disabled";
  if (evidence.feature != kDlssGFeatureId) return "not-DLSS-G";
  if (evidence.call_result != kNgxSuccess) return "original-call-failed";
  if (!evidence.ampere_adapter) return "adapter-not-qualified";
  if (evidence.prepared_providers != 1) return "provider-not-ready-or-ambiguous";
  if (evidence.flags != 0 && evidence.flags != kAdapterUnsupported)
    return "other-requirements-preserved";
  if (evidence.minimum_architecture != kAmpereArchitecture &&
      evidence.minimum_architecture != kAdaArchitecture) {
    return "unknown-minimum-architecture";
  }
  if (evidence.flags == 0 && evidence.minimum_architecture == kAmpereArchitecture)
    return "already-supported";
  return "architecture-override";
}

inline bool CanRelaxRequirements(const RequirementsEvidence& evidence) {
  // AdapterUnsupported is not an architecture-only error. Only relax it after
  // the real adapter is identified and exactly one provider has been prepared.
  return evidence.enabled && evidence.ampere_adapter && evidence.prepared_providers == 1 &&
         evidence.call_result == kNgxSuccess && evidence.feature == kDlssGFeatureId &&
         (evidence.flags == 0 || evidence.flags == kAdapterUnsupported) &&
         (evidence.minimum_architecture == kAdaArchitecture ||
          (evidence.minimum_architecture == kAmpereArchitecture &&
           evidence.flags == kAdapterUnsupported));
}

inline bool IsSupportedAmpere(uint32_t vendor, uint32_t architecture, uint32_t implementation) {
  return vendor == kNvidiaVendorId && architecture == kAmpereArchitecture &&
         (implementation == 2 || implementation == 4);
}

inline bool CanExposeAda(bool enabled, bool fg_requirements_scope, bool same_physical_gpu,
                         bool provider_ready, int nvapi_result, uint32_t architecture,
                         uint32_t implementation) {
  return enabled && fg_requirements_scope && same_physical_gpu && provider_ready &&
         nvapi_result == 0 &&
         IsSupportedAmpere(kNvidiaVendorId, architecture, implementation);
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
