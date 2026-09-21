// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

#include "../../../src/addons/mfgunlock/diagnostic_bridge.hpp"
#include "../../../src/addons/mfgunlock/count_observer.hpp"

namespace {
unsigned int g_checks = 0;
std::atomic_uint32_t g_eval_calls{0};
std::atomic_uint32_t g_lifecycle_calls{0};
std::atomic_uint32_t g_count_calls{0};

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (condition) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}
void Eval(const mfgunlock::diagnostic::EvaluateEvent*) noexcept { ++g_eval_calls; }
void Life(const mfgunlock::diagnostic::LifecycleEvent*) noexcept { ++g_lifecycle_calls; }
void Count(const mfgunlock::countobserver::Event*) noexcept { ++g_count_calls; }
void SelfDisconnectEval(const mfgunlock::diagnostic::EvaluateEvent*) noexcept {
  ++g_eval_calls;
  mfgunlock::diagnostic::SetEvaluateCallback(nullptr);
}
void ReenterEval(const mfgunlock::diagnostic::EvaluateEvent* event) noexcept {
  ++g_eval_calls;
  mfgunlock::diagnostic::Emit(*event);
  mfgunlock::diagnostic::LifecycleEvent nested;
  mfgunlock::diagnostic::Emit(nested);
  Check(!mfgunlock::diagnostic::SetLifecycleCallback(Life), "cross-kind replacement rejected without lock inversion");
  Check(!mfgunlock::diagnostic::SetEvaluateCallback(Eval), "reentrant callback replacement rejected");
  mfgunlock::diagnostic::SetEvaluateCallback(nullptr);
}
void ReenterLife(const mfgunlock::diagnostic::LifecycleEvent* event) noexcept {
  ++g_lifecycle_calls;
  mfgunlock::diagnostic::Emit(*event);
  Check(!mfgunlock::diagnostic::SetLifecycleCallback(Life), "reentrant lifecycle replacement rejected");
  mfgunlock::diagnostic::SetLifecycleCallback(nullptr);
}
void ReenterCount(const mfgunlock::countobserver::Event* event) noexcept {
  ++g_count_calls;
  mfgunlock::countobserver::Emit(*event);
  Check(!mfgunlock::countobserver::SetCallback(Count), "reentrant count replacement rejected");
  mfgunlock::countobserver::SetCallback(nullptr);
}
std::atomic_bool g_entered{false}, g_leave{false}, g_callback_done{false};
void BlockingEval(const mfgunlock::diagnostic::EvaluateEvent*) noexcept {
  g_entered.store(true, std::memory_order_release);
  while (!g_leave.load(std::memory_order_acquire)) std::this_thread::yield();
  g_callback_done.store(true, std::memory_order_release);
}
}  // namespace

int main() {
  namespace dg = mfgunlock::diagnostic;
  static_assert(std::is_standard_layout_v<dg::DiagnosticStateV1>);
  static_assert(std::is_standard_layout_v<dg::DiagnosticStateV2>);
  static_assert(std::is_standard_layout_v<dg::DiagnosticState>);

  Check(dg::kStateVersion1 == 1 && dg::kStateVersion2 == 2 && dg::kStateVersion == 3,
        "diagnostic state versions are explicit");
  Check(offsetof(dg::DiagnosticStateV2, architecture) ==
            offsetof(dg::DiagnosticStateV1, architecture),
        "v2 architecture preserves v1 offset");
  Check(offsetof(dg::DiagnosticStateV2, requested_generated_known) ==
            sizeof(dg::DiagnosticStateV1),
        "v2 fields append after the complete v1 payload");
  Check(offsetof(dg::DiagnosticState, architecture) ==
            offsetof(dg::DiagnosticStateV2, architecture),
        "v3 architecture preserves v2 offset");
  Check(offsetof(dg::DiagnosticState, automatic_uir_reject_reasons) ==
            offsetof(dg::DiagnosticStateV2, automatic_uir_reject_reasons),
        "v3 preserves complete v2 field offsets");
  Check(offsetof(dg::DiagnosticState, frame_contract) == sizeof(dg::DiagnosticStateV2),
        "v3 integration fields append after complete v2 payload");
  Check(sizeof(dg::DiagnosticState) > sizeof(dg::DiagnosticStateV2),
        "v3 extends rather than replaces v2");

  dg::DiagnosticState state;
  Check(state.requested_mode == dg::kUnknown32 &&
            state.forwarded_mode == dg::kUnknown32 &&
            state.accepted_mode == dg::kUnknown32 &&
            state.observed_mode_known == 0 && state.observed_mode == dg::kUnknown32,
        "unobserved modes stay unknown");
  Check(state.provider_applied_generated_known == 0 &&
            state.provider_applied_generated == 0,
        "provider-applied multiplier is not inferred from SetOptions acceptance");
  Check(state.dlssg_color_space_known == 0 &&
            state.dlssg_color_space == dg::kUnknown32,
        "unobserved DLSS-G color space stays unknown");
  Check(state.waitable_object_ownership_known == 0 && state.fullscreen_transition_known == 0 &&
            state.iflip_known == 0,
        "unsupported ownership evidence defaults to unknown");

  dg::SetEvaluateCallback(Eval);
  dg::SetLifecycleCallback(Life);
  mfgunlock::countobserver::SetCallback(Count);
  dg::EvaluateEvent eval;
  dg::LifecycleEvent life;
  mfgunlock::countobserver::Event count;
  dg::Emit(eval);
  dg::Emit(life);
  mfgunlock::countobserver::Emit(count);
  Check(g_eval_calls == 1 && g_lifecycle_calls == 1 && g_count_calls == 1,
        "registered observers receive events");
  dg::SetEvaluateCallback(nullptr);
  dg::SetLifecycleCallback(nullptr);
  mfgunlock::countobserver::SetCallback(nullptr);
  dg::Emit(eval);
  dg::Emit(life);
  mfgunlock::countobserver::Emit(count);
  Check(g_eval_calls == 1 && g_lifecycle_calls == 1 && g_count_calls == 1,
        "unregister stops callbacks without permanent module pinning");

  dg::SetEvaluateCallback(SelfDisconnectEval);
  dg::Emit(eval);
  dg::Emit(eval);
  Check(g_eval_calls == 2, "observer may disconnect itself without deadlocking");
  dg::SetEvaluateCallback(nullptr);

  dg::SetEvaluateCallback(ReenterEval);
  dg::SetLifecycleCallback(ReenterLife);
  mfgunlock::countobserver::SetCallback(ReenterCount);
  dg::Emit(eval);
  dg::Emit(life);
  mfgunlock::countobserver::Emit(count);
  Check(g_eval_calls == 3 && g_lifecycle_calls == 2 && g_count_calls == 2,
        "recursive observer emission is bounded, not a recursive shared-mutex acquisition");
  dg::SetEvaluateCallback(BlockingEval);
  std::thread emitter([&] { dg::Emit(eval); });
  while (!g_entered.load(std::memory_order_acquire)) std::this_thread::yield();
  std::atomic_bool drain_started{false}, drained_after_callback{false};
  std::thread drain([&] {
    drain_started.store(true, std::memory_order_release);
    dg::SetEvaluateCallback(nullptr);
    drained_after_callback.store(g_callback_done.load(std::memory_order_acquire));
  });
  while (!drain_started.load(std::memory_order_acquire)) std::this_thread::yield();
  g_leave.store(true, std::memory_order_release);
  emitter.join();
  drain.join();
  Check(drained_after_callback.load(), "external unregister drains an active callback");
  std::cout << "diagnostic_bridge_test: " << g_checks << " checks PASS\n";
  return 0;
}
