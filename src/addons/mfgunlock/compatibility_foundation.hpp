// SPDX-License-Identifier: MIT
#pragma once

// Shared, offline-testable evidence types used by compatibility_runtime.hpp.
// No hooks, loaders or DLL writes live here.
// A structural match is not a calling-ABI proof or a runtime patch permission.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mfgunlock::compatibility {

enum class CapabilityState : uint32_t {
  kUnknown,
  kUnavailable,
  kObserved,
  kStructuralMatch,
  kAmbiguous,
};

inline const char* Name(CapabilityState state) {
  switch (state) {
    case CapabilityState::kUnavailable: return "UNAVAILABLE";
    case CapabilityState::kObserved: return "OBSERVED";
    case CapabilityState::kStructuralMatch: return "STRUCTURAL_MATCH";
    case CapabilityState::kAmbiguous: return "AMBIGUOUS";
    default: return "UNKNOWN";
  }
}

struct StructuralEvidence {
  size_t candidate_count = 0;
  bool context_valid = false;
  bool invariants_valid = false;
};

struct Capability {
  CapabilityState state = CapabilityState::kUnknown;
  StructuralEvidence evidence{};
  std::string reason;
  // No implicit bool conversion: callers must name the evidence they require.
  bool HasStructuralEvidence() const { return state == CapabilityState::kStructuralMatch; }
};

inline Capability Evaluate(StructuralEvidence evidence, std::string reason = {}) {
  const auto state = evidence.candidate_count == 0 ? CapabilityState::kUnavailable :
                     evidence.candidate_count > 1 ? CapabilityState::kAmbiguous :
                     evidence.context_valid && evidence.invariants_valid ? CapabilityState::kStructuralMatch :
                     CapabilityState::kObserved;
  return {state, evidence, std::move(reason)};
}

struct LibraryIdentity {
  // Own strings: identities do not borrow memory from a loadable module.
  std::string path;
  std::string sha256;
  std::string file_version;
  std::string product_version;
  uint32_t pe_timestamp = 0;
  uint32_t image_bytes = 0;
  bool pe_valid = false;
};

struct PayloadDigest {
  uint64_t bytes = 0;
  std::array<unsigned char, 32> sha256{};
  bool operator==(const PayloadDigest&) const = default;
};

inline bool CompletePayloadMultisetEqual(std::span<const PayloadDigest> actual,
                                        std::span<const PayloadDigest> expected) {
  if (actual.empty() || expected.empty() || actual.size() != expected.size()) return false;
  const auto less = [](const PayloadDigest& a, const PayloadDigest& b) {
    if (a.bytes != b.bytes) return a.bytes < b.bytes;
    return a.sha256 < b.sha256;
  };
  std::vector<PayloadDigest> a(actual.begin(), actual.end());
  std::vector<PayloadDigest> b(expected.begin(), expected.end());
  std::sort(a.begin(), a.end(), less);
  std::sort(b.begin(), b.end(), less);
  return a == b;  // duplicates are significant; no RVA/version comparison
}

enum class ProviderRecognition : uint32_t { kUnknownSafe, kPartial, kStructural };
struct ProviderIdentity {
  LibraryIdentity library;
  std::string family;
  std::string payload_multiset_sha256;
  ProviderRecognition recognition = ProviderRecognition::kUnknownSafe;
};
struct ProviderCapabilities {
  Capability identity;
  Capability container_inventory;
  Capability retarget_payloads;
  Capability endpoint_code;
  Capability architecture_gates;
  Capability preset_code;
  Capability temporal_midpoint;
  Capability motion_vector;
  Capability inpaint;
  Capability inpaint_decision;
  Capability validated_warp;
  // The four exact quality signatures are independent, not a global support bit.
};

enum class StreamlineFamily : uint32_t { kUnknownSafe, kLegacySurface, kModernSurface, kAmbiguous };
inline const char* Name(StreamlineFamily family) {
  switch (family) {
    case StreamlineFamily::kLegacySurface: return "legacy-export-surface";
    case StreamlineFamily::kModernSurface: return "modern-export-surface";
    case StreamlineFamily::kAmbiguous: return "ambiguous-export-surface";
    default: return "unknown-safe";
  }
}
struct ExportSymbol {
  std::string_view name;
  bool own_code = false;
};
inline size_t CountOwnCode(std::span<const ExportSymbol> symbols, std::string_view name) {
  return static_cast<size_t>(std::count_if(symbols.begin(), symbols.end(), [&](const auto& symbol) {
    return symbol.own_code && symbol.name == name;
  }));
}
inline StreamlineFamily ClassifyStreamline(std::span<const ExportSymbol> symbols) {
  // The public resolver identifies the modern adapter. Optional version,
  // requirements, tag and lifecycle exports do not need to travel together;
  // each installed hook still checks its own executable address and lifetime.
  // Compatibility exports from the older surface do not invalidate a modern
  // resolver that can actually be called.
  const size_t modern = CountOwnCode(symbols, "slGetFeatureFunction");
  if (modern > 1) return StreamlineFamily::kAmbiguous;
  if (modern == 1) return StreamlineFamily::kModernSurface;
  const size_t settings = CountOwnCode(symbols, "slGetFeatureSettings");
  const size_t constants = CountOwnCode(symbols, "slSetFeatureConstants");
  if (settings > 1 || constants > 1) return StreamlineFamily::kAmbiguous;
  if (settings == 1 && constants == 1) return StreamlineFamily::kLegacySurface;
  return StreamlineFamily::kUnknownSafe;
}

struct StreamlineIdentity {
  LibraryIdentity interposer;
  LibraryIdentity common;
  LibraryIdentity dlssg_plugin;
  StreamlineFamily family = StreamlineFamily::kUnknownSafe;
};
struct AdmissionCapabilities {
  Capability feature_function_export;
  Capability common_gateway;
  Capability dlssg_gateway;
  Capability tag_for_frame_export;
  Capability direct_ngx_d3d12;
  Capability direct_ngx_vulkan;
  // Remains unknown in an offline scan, even with every expected export.
  Capability calling_abi;
};
struct RuntimeCapabilities {
  Capability initialization;
  Capability frame_generation;
  Capability multi_frame_generation;
  Capability dynamic_mfg;
  Capability ui_recomposition;
  Capability target_preparation;
};
struct CompatibilityState {
  ProviderIdentity provider;
  ProviderCapabilities provider_capabilities;
  StreamlineIdentity streamline;
  AdmissionCapabilities admission;
  RuntimeCapabilities runtime;
  // Diagnostic usability is independent of unsafe provider feature support.
  bool diagnostics_available = true;
  bool runtime_wiring_present = false;  // Stage 1 does not change production behavior.
};

}  // namespace mfgunlock::compatibility
