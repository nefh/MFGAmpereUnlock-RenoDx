// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/presentation_observer.hpp"

#include <cstdio>
#include <cstdlib>

namespace p = mfgunlock::presentation;

namespace {
unsigned int g_checks = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}

void Present(uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) p::ObserveApplicationPresent();
}

void Baseline() {
  p::ObserveSourceBinding(0, true);
  p::ObserveProviderPresentation(0);
}
}  // namespace

int main() {
  p::Reset();
  p::ObserveProviderOptions(p::ProviderMode::kFixed, 4);
  Baseline();
  Present(32);
  p::ObserveProviderPresentation(128);
  auto state = p::Read();
  Check(state.effective_multiplier_known && state.effective_multiplier_milli == 4000,
        "fixed 4x effective multiplier");
  Check(state.confidence == p::Confidence::kReliable,
        "fixed 4x reliable confidence");
  Check(state.health == p::Health::kHealthy,
        "fixed 4x healthy presentation");
  Check(state.presented_interpolated_known && state.presented_interpolated_frames == 96,
        "fixed 4x presented interpolated frames");
  Check(state.dropped_interpolated_known && state.dropped_interpolated_frames == 0,
        "fixed 4x no dropped frames");
  Check(!state.generated_multiplier_known,
        "actual generated multiplier remains unknown");

  p::ObserveProviderOptions(p::ProviderMode::kFixed, 4);
  p::ClearCorrelation();
  Baseline();
  Present(32);
  p::ObserveProviderPresentation(64);
  state = p::Read();
  Check(state.effective_multiplier_known && state.effective_multiplier_milli == 2000,
        "presentation starvation effective 2x");
  Check(state.health == p::Health::kStarved,
        "presentation starvation classified separately");
  Check(state.dropped_interpolated_known && state.dropped_interpolated_frames == 64,
        "presentation starvation dropped estimate");

  p::ObserveProviderOptions(p::ProviderMode::kDynamic, 4);
  Baseline();
  Present(32);
  p::ObserveProviderPresentation(80);
  state = p::Read();
  Check(state.effective_multiplier_known && state.effective_multiplier_milli == 2500,
        "dynamic path still measures presentation");
  Check(state.health == p::Health::kUnknown,
        "dynamic path does not infer starvation");
  Check(!state.dropped_interpolated_known,
        "dynamic path does not invent dropped count");

  const uint32_t resets_before = state.reset_count;
  p::ClearCorrelation();
  state = p::Read();
  Check(!state.effective_multiplier_known &&
        state.confidence == p::Confidence::kUnknown,
        "correlation reset clears evidence");
  Check(state.reset_count == resets_before + 1,
        "correlation reset counted");

  p::ObserveDxgi(true,
                 true, p::SwapEffect::kFlipDiscard,
                 true, p::WindowMode::kWindowedOrBorderless,
                 true, true,
                 true, true,
                 true, 2,
                 true, true);
  state = p::Read();
  Check(state.dxgi_known && state.swap_effect_known &&
        state.swap_effect == p::SwapEffect::kFlipDiscard,
        "DXGI swap effect evidence");
  Check(state.waitable_flag_known && state.waitable_flag &&
        state.waitable_object_known && state.waitable_object_available,
        "waitable swapchain evidence");
  Check(state.max_frame_latency_known && state.max_frame_latency == 2,
        "max frame latency evidence");
  Check(state.allow_tearing_flag_known && state.allow_tearing_flag,
        "allow tearing creation flag evidence");
  p::ObserveDxgi(true,
                 true, p::SwapEffect::kFlipDiscard,
                 true, p::WindowMode::kExclusiveFullscreen,
                 true, true,
                 true, true,
                 true, 1,
                 true, true);
  state = p::Read();
  Check(state.window_mode_transition_count == 1,
        "window mode transition counted");
  Check(state.max_frame_latency_change_count == 1,
        "max frame latency transition counted");
  Check(!state.sync_interval_known && !state.iflip_known,
        "unobservable presentation details stay unknown");

  p::Reset();
  p::ObserveProviderOptions(p::ProviderMode::kFixed, 4);
  Baseline();
  Present(1);
  p::ObserveProviderPresentation(1);
  p::ObserveProviderPresentation(3);  // Pacer completes without another application Present.
  state = p::Read();
  Check(state.correlated_source_presents == 1 && state.correlated_provider_presented == 4 &&
        state.effective_multiplier_milli == 4000, "zero-source GetState retains consumed frames");
  Check(state.health == p::Health::kUnknown && state.confidence == p::Confidence::kObserved,
        "single asynchronous group is not reliable pacing evidence");
  p::ObserveProviderUnavailable();
  state = p::Read();
  Check(!state.accepted_mode_known && !state.accepted_multiplier_known,
        "unavailable clears both mode and multiplier, without short circuit");
  p::Reset();
  p::ObserveProviderOptions(p::ProviderMode::kFixed, 4, 5);
  p::ObserveProviderPresentation(0, 5);
  Present(32);
  p::ObserveProviderPresentation(128, 6);
  state = p::Read();
  Check(!state.effective_multiplier_known && state.evidence_reason == p::EvidenceReason::kViewportMismatch,
        "mixed viewport counters cannot be correlated");
  p::ObserveProviderPresentation(128, 5);
  Check(!p::Read().effective_multiplier_known, "viewport ambiguity stays fail-closed until reset");
  p::Reset();
  p::ObserveProviderOptions(p::ProviderMode::kFixed, 4);
  p::ObserveProviderPresentation(0);
  Present(64);
  p::ObserveProviderPresentation(256);
  state = p::Read();
  Check(state.confidence == p::Confidence::kObserved && state.health == p::Health::kUnknown &&
        !state.dropped_interpolated_known && !state.source_binding_verified &&
        state.evidence_reason == p::EvidenceReason::kSourceBindingUnknown,
        "many samples cannot prove an unbound source/provider identity");
  p::ObserveSourceBinding(0, true);
  Check(!p::Read().effective_multiplier_known, "binding change starts a fresh correlation interval");
  Baseline();
  Present(32);
  p::ObserveProviderPresentation(128);
  Check(p::Read().confidence == p::Confidence::kReliable, "verified binding permits delivery estimate");
  p::ObserveSourceBinding(0, false);
  Check(!p::Read().effective_multiplier_known && !p::Read().source_binding_verified,
        "lost binding invalidates the delivery estimate");
  Check(p::ClampMilli(UINT64_MAX, UINT64_MAX) == 1000,
        "ratio arithmetic does not overflow at 64-bit counter range");
  Check(p::ClampMilli(UINT64_MAX, 1) == UINT32_MAX, "oversized ratios saturate");

  std::printf("presentation_observer_test PASS (%u checks)\n", g_checks);
  return 0;
}
