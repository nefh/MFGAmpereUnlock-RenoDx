// SPDX-License-Identifier: MIT
#pragma once

// Runtime decisions built on the Stage 1 evidence vocabulary. A callable
// operation selects its existing typed adapter independently of unrelated
// module exports. File versions and capability observations are diagnostics;
// an actual attempted operation supplies the runtime result.

#include "./compatibility_foundation.hpp"

namespace mfgunlock::compatibility {

enum class FeatureState : uint32_t { kUnknown, kUnavailable, kDegraded, kAvailable };
inline const char* Name(FeatureState state) {
  switch (state) {
    case FeatureState::kUnavailable: return "UNAVAILABLE";
    case FeatureState::kDegraded: return "DEGRADED";
    case FeatureState::kAvailable: return "AVAILABLE";
    default: return "UNKNOWN";
  }
}

struct FeatureDecision {
  FeatureState state = FeatureState::kUnknown;
  const char* reason = "evidence has not been observed";
  bool Available() const { return state == FeatureState::kAvailable; }
};

enum class TypedInterposerAbi : uint32_t { kNone, kLegacy1, kModern2 };
struct InterposerContract {
  StreamlineFamily surface = StreamlineFamily::kUnknownSafe;
  TypedInterposerAbi typed_abi = TypedInterposerAbi::kNone;
  FeatureDecision admission{};
};

inline InterposerContract AssessInterposer(std::span<const ExportSymbol> symbols,
                                           bool common_present, bool current_image) {
  (void)common_present;  // Loading a sibling is not a calling-ABI discriminator.
  InterposerContract result;
  result.surface = ClassifyStreamline(symbols);
  if (!current_image) {
    result.admission = {FeatureState::kUnavailable, "interposer image changed or disappeared"};
  } else if (result.surface == StreamlineFamily::kModernSurface) {
    result.typed_abi = TypedInterposerAbi::kModern2;
    result.admission = {FeatureState::kAvailable, "MODERN_EXPORT_CONTRACT"};
  } else if (result.surface == StreamlineFamily::kLegacySurface) {
    result.typed_abi = TypedInterposerAbi::kLegacy1;
    result.admission = {FeatureState::kAvailable, "LEGACY_EXPORT_CONTRACT"};
  } else {
    result.admission = {FeatureState::kUnknown, "export surface does not qualify a typed calling ABI"};
  }
  return result;
}

struct PluginGatewayEvidence {
  bool named_dlssg = false;
  bool own_code_gateway = false;
  bool current_image = false;
  bool set_options = false;
  bool get_state = false;
  bool on_load = false;
  bool on_startup = false;
};

inline FeatureDecision GatewayLookup(const PluginGatewayEvidence& e) {
  if (!e.current_image || !e.own_code_gateway)
    return {FeatureState::kUnavailable, "plugin gateway is absent, forwarded, or stale"};
  if (!e.named_dlssg)
    return {FeatureState::kUnknown, "not a DLSS-G plugin role"};
  // slGetPluginFunction has its own stable opaque-pointer ABI. Its native
  // lookup must not depend on every unrelated export in the parent module.
  return {FeatureState::kAvailable, "matching typed plugin gateway contract"};
}

inline FeatureDecision PluginFunctions(const PluginGatewayEvidence& e, TypedInterposerAbi abi) {
  const auto gateway = GatewayLookup(e);
  if (!gateway.Available()) return gateway;
  if (e.set_options || e.get_state)
    return {FeatureState::kAvailable, "individual typed DLSS-G operation entrypoint"};
  if (abi == TypedInterposerAbi::kLegacy1 && e.on_load && e.on_startup)
    return {FeatureState::kDegraded, "legacy lifecycle observed; options/state entrypoints absent"};
  return {FeatureState::kUnavailable, "required DLSS-G plugin functions are missing"};
}

struct DynamicEvidence {
  // These two fields describe the individual callable entrypoint, including
  // its current module lifetime. One operation must not require the other.
  bool live_set_options = false;
  bool live_get_state = false;
  bool architecture_supports = false;
  bool target_ready = false;
  bool provider_qualified = false;
  bool typed_modern_interposer = false;
  bool typed_modern_plugin = false;
  bool options_v5 = false;  // capacity of our owned typed request, not caller ABI
  bool state_v4 = false;    // capacity of our owned typed query, not caller ABI
  bool hook_entries_current = false;
  bool ceiling_observed = false;
  bool d3d12 = false;
  bool api_support_observed = false;
  bool api_supports_dynamic = false;
};

inline FeatureDecision DynamicStack(const DynamicEvidence& e) {
  if (!e.live_set_options)
    return {FeatureState::kDegraded, "live slDLSSGSetOptions entrypoint is absent"};
  if (!e.options_v5)
    return {FeatureState::kDegraded, "owned DLSSGOptions v5 request is unavailable"};
  // A false/unknown capability bit is not a failed eDynamic request. Vulkan,
  // architecture profiles, provider preparation and a prior fixed ceiling are
  // not calling prerequisites. The caller records the native attempt result.
  return {FeatureState::kAvailable, "live typed Dynamic request can be attempted"};
}

// Query the individual getter even when no setter, profile, capability, or
// unrelated hook has been observed. Never use the query result as permission
// for the user's separately selected Dynamic request.
inline FeatureDecision DynamicProbe(const DynamicEvidence& e) {
  if (!e.live_get_state)
    return {FeatureState::kDegraded, "live slDLSSGGetState entrypoint is absent"};
  if (!e.state_v4)
    return {FeatureState::kDegraded, "owned DLSSGState v4 query is unavailable"};
  return {FeatureState::kAvailable, "live typed Dynamic capability query can be attempted"};
}

inline FeatureDecision LateAttach(bool interposer_present, bool init_observed,
                                  bool feature_create_observed) {
  if (!interposer_present)
    return {FeatureState::kUnavailable, "Streamline interposer is not loaded"};
  if (!init_observed) {
    // Absence of an observation is not proof that startup already happened.
    // Feature creation is positive evidence of activity, not permission to
    // replay slInit or to reconstruct unobserved Preferences.
    return feature_create_observed
        ? FeatureDecision{FeatureState::kDegraded,
                          "startup not observed; feature activity seen; no replay"}
        : FeatureDecision{FeatureState::kUnknown,
                          "startup not observed; waiting or late attach"};
  }
  if (!feature_create_observed)
    return {FeatureState::kUnknown, "feature-create has not been observed"};
  return {FeatureState::kAvailable, "init and feature-create were observed"};
}

struct ModuleGeneration {
  uintptr_t base = 0;
  uint32_t timestamp = 0;
  uint32_t image_bytes = 0;
  uint64_t epoch = 0;
  bool live = false;

  bool Observe(uintptr_t next_base, uint32_t next_timestamp, uint32_t next_bytes) {
    const bool next_live = next_base != 0 && next_bytes != 0;
    if (base == next_base && timestamp == next_timestamp && image_bytes == next_bytes &&
        live == next_live) return false;
    base = next_base;
    timestamp = next_timestamp;
    image_bytes = next_bytes;
    live = next_live;
    ++epoch;
    return true;
  }
};

}  // namespace mfgunlock::compatibility
