// SPDX-License-Identifier: MIT
#include "ui_candidate.hpp"

#include <cstdio>
#include <cstdlib>

namespace policy = mfgunlock::uicandidate;
namespace {
unsigned int g_checks = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}

policy::Evidence ValidEvidence() {
  policy::Evidence evidence{};
  evidence.output_match = true;
  evidence.rgba8_with_alpha = true;
  evidence.fresh_transparent_clear = true;
  evidence.state_known = true;
  evidence.recent = true;
  evidence.rtv_binds_after_clear = 5;
  return evidence;
}
}  // namespace

int main() {
  auto evidence = ValidEvidence();
  Check(policy::Assess(evidence) == policy::kAccept,
        "validated UI candidate signature is accepted");
  Check(!policy::IsStable(policy::kRequiredStableFrames - 1) &&
            policy::IsStable(policy::kRequiredStableFrames),
        "stability gate requires the configured consecutive frames");
  Check(policy::CanWarmHandover(true, true, policy::kHandoverMaxGapMs) &&
            !policy::CanWarmHandover(true, true, policy::kHandoverMaxGapMs + 1) &&
            !policy::CanWarmHandover(false, true, 0),
        "logical UI family handover requires a matching recent family");
  Check(policy::HandoverSeedStableFrames() == policy::kRequiredStableFrames - 1,
        "warm handover requires two confirming source frames");
  Check(policy::NextFailureStreak(false, 3) == 1 &&
            policy::NextFailureStreak(true, 3) == 4,
        "failure streak resets when the physical UI resource changes");
  Check(!policy::ShouldHardDecline(policy::kMaxConsecutiveInjectionFailures - 1) &&
            policy::ShouldHardDecline(policy::kMaxConsecutiveInjectionFailures),
        "one transient tag failure cannot disable injection for the session");
  Check(policy::CanInject(evidence, policy::kRequiredStableFrames,
                          true, true, true),
        "stable candidate can inject when command buffer and options are ready");
  Check(!policy::CanInject(evidence, policy::kRequiredStableFrames,
                           false, true, true),
        "missing command buffer fails closed");
  Check(!policy::CanInject(evidence, policy::kRequiredStableFrames,
                           true, false, true),
        "unsynchronized UI options fail closed");
  Check(!policy::CanInject(evidence, policy::kRequiredStableFrames,
                           true, true, false),
        "disabled UI injection remains fail-closed");

  evidence = ValidEvidence();
  evidence.output_match = false;
  Check((policy::Assess(evidence) & policy::kWrongOutput) != 0,
        "output mismatch is rejected");
  evidence = ValidEvidence();
  evidence.swapchain = true;
  Check((policy::Assess(evidence) & policy::kSwapchain) != 0,
        "swapchain resource is rejected");
  evidence = ValidEvidence();
  evidence.already_tagged = true;
  Check((policy::Assess(evidence) & policy::kAlreadyTagged) != 0,
        "already-tagged resource is rejected");
  evidence = ValidEvidence();
  evidence.rgba8_with_alpha = false;
  Check((policy::Assess(evidence) & policy::kWrongFormat) != 0,
        "non-RGBA8 candidate is rejected");
  evidence = ValidEvidence();
  evidence.fresh_transparent_clear = false;
  Check((policy::Assess(evidence) & policy::kNoFreshTransparentClear) != 0,
        "missing fresh transparent clear is rejected");
  evidence = ValidEvidence();
  evidence.rtv_binds_after_clear = policy::kMinRtvBindsAfterClear - 1;
  Check((policy::Assess(evidence) & policy::kUnexpectedPassCount) != 0,
        "too few render-target passes are rejected");
  evidence = ValidEvidence();
  evidence.rtv_binds_after_clear = policy::kMaxRtvBindsAfterClear + 1;
  Check((policy::Assess(evidence) & policy::kUnexpectedPassCount) != 0,
        "too many render-target passes are rejected");
  evidence = ValidEvidence();
  evidence.state_known = false;
  Check((policy::Assess(evidence) & policy::kStateUnknown) != 0,
        "unknown D3D12 state is rejected");
  evidence = ValidEvidence();
  evidence.recent = false;
  Check((policy::Assess(evidence) & policy::kStale) != 0,
        "stale candidate is rejected");
  evidence = ValidEvidence();
  evidence.late_writes = 1;
  Check((policy::Assess(evidence) & policy::kLateWrite) != 0,
        "writes after the previous HUD-less boundary fail closed");

  std::printf("PASS UI candidate policy: %u checks\n", g_checks);
}
