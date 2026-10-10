// SPDX-License-Identifier: MIT
#pragma once

// Runtime decisions built on the Stage 1 evidence vocabulary. Select existing
// typed adapters from the complete own-code export contract. File versions are
// diagnostic metadata, not admission criteria; native results stay authoritative.

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
  InterposerContract result;
  result.surface = ClassifyStreamline(symbols);
  if (!current_image) {
    result.admission = {FeatureState::kUnavailable, "interposer image changed or disappeared"};
  } else if (!common_present) {
    result.admission = {FeatureState::kDegraded, "sl.common is missing"};
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
  bool parent_abi_qualified = false;
  bool set_options = false;
  bool get_state = false;
  bool on_load = false;
  bool on_startup = false;
};

inline FeatureDecision GatewayLookup(const PluginGatewayEvidence& e) {
  if (!e.current_image || !e.own_code_gateway)
    return {FeatureState::kUnavailable, "plugin gateway is absent, forwarded, or stale"};
  if (!e.named_dlssg || !e.parent_abi_qualified)
    return {FeatureState::kUnknown, "plugin and interposer gateway ABI not qualified"};
  return {FeatureState::kAvailable, "matching typed plugin gateway contract"};
}

inline FeatureDecision PluginFunctions(const PluginGatewayEvidence& e, TypedInterposerAbi abi) {
  const auto gateway = GatewayLookup(e);
  if (!gateway.Available()) return gateway;
  if (abi == TypedInterposerAbi::kModern2 && e.set_options && e.get_state)
    return {FeatureState::kAvailable, "typed DLSS-G options and state entrypoints"};
  if (abi == TypedInterposerAbi::kLegacy1 && e.on_load && e.on_startup)
    return {FeatureState::kDegraded, "legacy lifecycle only; Dynamic MFG unqualified"};
  return {FeatureState::kUnavailable, "required DLSS-G plugin functions are missing"};
}

struct DynamicEvidence {
  bool architecture_supports = false;
  bool target_ready = false;
  bool provider_qualified = false;
  bool typed_modern_interposer = false;
  bool typed_modern_plugin = false;
  bool options_v5 = false;
  bool state_v4 = false;
  bool hook_entries_current = false;
  bool ceiling_observed = false;
  bool d3d12 = false;
  bool api_support_observed = false;
  bool api_supports_dynamic = false;
};

inline FeatureDecision DynamicStack(const DynamicEvidence& e) {
  if (!e.architecture_supports || !e.d3d12)
    return {FeatureState::kUnavailable, "target/backend does not support Dynamic MFG"};
  if (!e.provider_qualified || !e.target_ready)
    return {FeatureState::kDegraded, "provider or target preparation is unqualified"};
  if (!e.typed_modern_interposer || !e.typed_modern_plugin)
    return {FeatureState::kDegraded, "typed Streamline/plugin contract is unavailable"};
  if (!e.options_v5 || !e.state_v4 || !e.hook_entries_current)
    return {FeatureState::kDegraded, "Options/State ABI or live entrypoints unqualified"};
  if (!e.ceiling_observed)
    return {FeatureState::kDegraded, "plugin frame-count capability unobserved"};
  if (!e.api_support_observed)
    return {FeatureState::kUnknown, "waiting for successful slDLSSGGetState v4"};
  if (!e.api_supports_dynamic)
    return {FeatureState::kUnavailable, "slDLSSGGetState reports Dynamic MFG unsupported"};
  return {FeatureState::kAvailable, "typed API and provider capability confirmed"};
}

// The v4 query itself must be allowed before support is observed. Its inputs
// undergo the same checks, but no support result is required to make the query.
inline FeatureDecision DynamicProbe(DynamicEvidence e) {
  e.api_support_observed = true;
  e.api_supports_dynamic = true;
  return DynamicStack(e);
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
