// SPDX-License-Identifier: MIT
#include "framecount.hpp"

#include <cstdio>
#include <cstdlib>

namespace fc = mfgunlock::framecount;
namespace {
unsigned int g_checks = 0;
unsigned int g_acquires = 0;
unsigned int g_releases = 0;
unsigned int g_forward_calls = 0;
unsigned int g_seen_count = 0;
bool g_fail_augmented = false;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}

bool AcquireCandidate(const fc::UiCandidateSnapshot*) {
  ++g_acquires;
  return true;
}

void ReleaseCandidate(const fc::UiCandidateSnapshot*, bool, sl::Result) {
  ++g_releases;
}

void Reset() {
  g_acquires = 0;
  g_releases = 0;
  g_forward_calls = 0;
  g_seen_count = 0;
  g_fail_augmented = false;
  fc::g_ui_candidate_injection_enabled = true;
  fc::g_ui_candidate_active = false;
  fc::g_ui_candidate_ready = false;
  fc::g_ui_candidate_format = 0;
  fc::g_ui_candidate_runtime_declined = false;
  fc::g_ui_candidate_retry_frames = 0;
  fc::g_ui_candidate_consecutive_failures = 0;
  fc::g_ui_candidate_last_failed_resource = 0;
  fc::g_ui_candidate_options_synced = true;
  fc::g_ui_candidate_tags_injected = 0;
  fc::g_ui_candidate_injection_failures = 0;
  fc::g_ui_candidate_recoveries = 0;
  fc::g_ui_candidate_native_fallbacks = 0;
  fc::g_ui_candidate_missing_command_buffer = 0;
  fc::g_ui_candidate_full_batches = 0;
  fc::g_ui_candidate_last_result = 0;
  fc::g_acquire_ui_candidate = AcquireCandidate;
  fc::g_release_ui_candidate = ReleaseCandidate;
}

fc::UiCandidateSnapshot Candidate() {
  fc::UiCandidateSnapshot candidate{};
  candidate.native_resource = 0x12340000;
  candidate.width = 2560;
  candidate.height = 1440;
  candidate.format = 29;
  candidate.state = 0x40;
  candidate.stable_frames = mfgunlock::uicandidate::kRequiredStableFrames;
  candidate.rtv_binds_after_clear = 5;
  candidate.reject_reasons = mfgunlock::uicandidate::kAccept;
  candidate.clear_serial = 77;
  candidate.state_known = true;
  candidate.recent = true;
  return candidate;
}
}  // namespace

int main() {
  Reset();
  fc::g_ui_candidate_ready = true;
  fc::g_ui_candidate_format = 29;
  sl::DLSSGOptions source{};
  source.structVersion = sl::kStructVersion3;
  source.uiBufferFormat = 0;
  sl::DLSSGOptions forwarded{};
  Check(fc::internal::BuildUiCompositionOptions(source, forwarded) &&
            forwarded.structVersion == sl::kStructVersion4 &&
            forwarded.uiBufferFormat == 29 &&
            forwarded.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
        "candidate format is synchronized through addon-owned UIR options");

  const uint64_t revision_before_format = fc::g_options_revision.load();
  fc::g_ui_candidate_ready = false;
  fc::g_ui_candidate_format = 29;
  fc::g_ui_candidate_options_synced = true;
  fc::internal::UpdateUiCandidateFormat(29);
  Check(fc::g_ui_candidate_ready.load() && fc::g_ui_candidate_format.load() == 29 &&
            !fc::g_ui_candidate_options_synced.load() &&
            fc::g_options_revision.load() == revision_before_format + 1,
        "candidate availability schedules a fresh runtime UIR options submission");
  const uint64_t revision_after_format = fc::g_options_revision.load();
  fc::internal::UpdateUiCandidateFormat(29);
  Check(fc::g_options_revision.load() == revision_after_format,
        "unchanged candidate format does not resubmit options every frame");
  fc::g_ui_candidate_options_synced = true;
  fc::internal::NotifyUiCandidateUnavailable();
  Check(!fc::g_ui_candidate_ready.load() &&
            fc::g_ui_candidate_format.load() == 0 &&
            !fc::g_ui_candidate_options_synced.load() &&
            fc::g_options_revision.load() == revision_after_format,
        "candidate loss invalidates readiness without submitting disabled options");
  fc::internal::UpdateUiCandidateFormat(29);
  Check(fc::g_ui_candidate_ready.load() &&
            fc::g_options_revision.load() == revision_after_format + 1,
        "same-format candidate reacquisition creates one new options revision");
  const uint64_t revision_after_reacquire = fc::g_options_revision.load();
  fc::internal::UpdateUiCandidateFormat(29);
  Check(fc::g_options_revision.load() == revision_after_reacquire,
        "stable reacquired candidate does not create a SetOptions storm");
  fc::g_ui_candidate_options_synced = true;

  sl::Resource hudless(sl::ResourceType::eTex2d,
                       reinterpret_cast<void*>(0x56780000), 0x40);
  hudless.width = 2560;
  hudless.height = 1440;
  hudless.nativeFormat = 28;
  sl::Extent extent{};
  extent.width = 2560;
  extent.height = 1440;
  sl::ResourceTag hudless_tag(&hudless, sl::kBufferTypeHUDLessColor,
                              sl::ResourceLifecycle::eValidUntilPresent, &extent);
  auto candidate = Candidate();
  auto forward = [&](const sl::ResourceTag* tags, uint32_t count) {
    ++g_forward_calls;
    g_seen_count = count;
    if (count == 2) {
      Check(tags[1].type == sl::kBufferTypeUIColorAndAlpha,
            "injected tag is UI Color+Alpha");
      Check(tags[1].lifecycle == sl::ResourceLifecycle::eOnlyValidNow,
            "injected UI resource is volatile");
      Check(tags[1].extent.width == 2560 && tags[1].extent.height == 1440,
            "injected UI extent matches the output");
      Check(tags[1].resource != nullptr && tags[1].resource->nativeFormat == 29 &&
                tags[1].resource->state == 0x40 &&
                tags[1].resource->native == reinterpret_cast<void*>(0x12340000),
            "injected UI resource carries the validated native metadata");
      if (g_fail_augmented) return sl::Result::eErrorUnsupportedInterface;
    }
    return sl::Result::eOk;
  };
  auto* commands = reinterpret_cast<sl::CommandBuffer*>(0x99);
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, forward) == sl::Result::eOk &&
            g_forward_calls == 1 && g_seen_count == 2 && g_acquires == 1 &&
            g_releases == 1 && fc::g_ui_candidate_tags_injected.load() == 1,
        "stable candidate appends one UI tag exactly once");

  Reset();
  candidate = Candidate();
  g_fail_augmented = true;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, forward) == sl::Result::eOk &&
            g_forward_calls == 2 && !fc::g_ui_candidate_runtime_declined.load() &&
            fc::g_ui_candidate_injection_failures.load() == 1 &&
            fc::g_ui_candidate_native_fallbacks.load() == 1 &&
            fc::g_ui_candidate_consecutive_failures.load() == 1 &&
            fc::g_ui_candidate_retry_frames.load() ==
                mfgunlock::uicandidate::kRetryCooldownFrames,
        "one runtime rejection falls back without permanently disabling injection");

  const unsigned int calls_after_failure = g_forward_calls;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, forward) == sl::Result::eOk &&
            g_forward_calls == calls_after_failure + 1 &&
            fc::g_ui_candidate_injection_failures.load() == 1,
        "retry cooldown forwards native HUD-less without another augmented call");

  fc::g_ui_candidate_retry_frames = 0;
  g_fail_augmented = false;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, forward) == sl::Result::eOk &&
            fc::g_ui_candidate_active.load() &&
            fc::g_ui_candidate_consecutive_failures.load() == 0 &&
            fc::g_ui_candidate_recoveries.load() == 1,
        "a later valid candidate recovers automatically after a transient rejection");

  Reset();
  candidate = Candidate();
  g_fail_augmented = true;
  fc::g_ui_candidate_last_failed_resource = candidate.native_resource;
  fc::g_ui_candidate_consecutive_failures =
      mfgunlock::uicandidate::kMaxConsecutiveInjectionFailures - 1;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, forward) == sl::Result::eOk &&
            fc::g_ui_candidate_runtime_declined.load(),
        "repeated failures on one physical resource eventually fail closed");

  Reset();
  candidate = Candidate();
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, nullptr, forward) == sl::Result::eOk &&
            g_forward_calls == 1 && g_seen_count == 1 && g_acquires == 0 &&
            fc::g_ui_candidate_missing_command_buffer.load() == 1,
        "missing command buffer preserves the native HUD-less submission");

  std::printf("PASS UI injection path: %u checks\n", g_checks);
}
