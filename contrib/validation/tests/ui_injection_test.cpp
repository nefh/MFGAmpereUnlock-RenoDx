// SPDX-License-Identifier: MIT
#include "framecount.hpp"

#include <cstdio>
#include <cstdlib>

namespace fc = mfgunlock::framecount;
namespace output = mfgunlock::outputstate;
namespace {
unsigned int g_checks = 0;
unsigned int g_acquires = 0;
unsigned int g_releases = 0;
unsigned int g_alpha_prepares = 0;
unsigned int g_alpha_finishes = 0;
unsigned int g_forward_calls = 0;
unsigned int g_seen_count = 0;
unsigned int g_legacy_clear_calls = 0;
sl::BufferType g_legacy_clear_type = 0;
bool g_fail_augmented = false;
bool g_fail_alpha_finish = false;
bool g_bad_alpha_metadata = false;

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

bool PrepareAlphaCandidate(const fc::UiCandidateSnapshot* source, sl::CommandBuffer*,
                           fc::UiAlphaCandidateSnapshot* output) {
  ++g_alpha_prepares;
  if (source == nullptr || output == nullptr) return false;
  output->native_resource = 0xABCDEF00;
  output->width = source->width + (g_bad_alpha_metadata ? 1u : 0u);
  output->height = source->height;
  output->format = 61;
  output->state = 0x40;
  output->copy_commands = reinterpret_cast<sl::CommandBuffer*>(0xCAFE);
  output->ticket = 7;
  output->slot = 0;
  return true;
}

bool FinishAlphaCandidate(const fc::UiCandidateSnapshot*,
                          const fc::UiAlphaCandidateSnapshot*, bool accepted, sl::Result result) {
  ++g_alpha_finishes;
  return accepted && result == sl::Result::eOk && !g_fail_alpha_finish;
}

sl::Result LegacySetTag(const sl::ViewportHandle&, const sl::ResourceTag* tags,
                        uint32_t count, sl::CommandBuffer*) {
  ++g_legacy_clear_calls;
  if (tags != nullptr && count == 1 && tags[0].resource == nullptr)
    g_legacy_clear_type = tags[0].type;
  return sl::Result::eOk;
}

void Reset() {
  g_acquires = 0;
  g_releases = 0;
  g_alpha_prepares = 0;
  g_alpha_finishes = 0;
  g_forward_calls = 0;
  g_seen_count = 0;
  g_legacy_clear_calls = 0;
  g_legacy_clear_type = 0;
  g_fail_augmented = false;
  g_fail_alpha_finish = g_bad_alpha_metadata = false;
  fc::g_ui_candidate_injection_enabled = true;
  fc::g_ui_candidate_active = false;
  fc::g_ui_candidate_ready = false;
  fc::g_ui_candidate_format = 0;
  fc::g_ui_candidate_path = static_cast<uint32_t>(mfgunlock::uisubmission::Path::kUnavailable);
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
  fc::g_prepare_ui_alpha_candidate = nullptr;
  fc::g_finish_ui_alpha_candidate = nullptr;
  output::Observe(output::FormatKind::kRgba8Unorm, 28, true,
                  output::kColorSpaceSrgbNonlinear, true);
}

fc::UiCandidateSnapshot Candidate() {
  fc::UiCandidateSnapshot candidate{};
  candidate.native_resource = 0x12340000;
  candidate.width = 2560;
  candidate.height = 1440;
  candidate.format = 29;
  candidate.ui_encoding = static_cast<uint32_t>(output::UiEncoding::kSdrSrgb);
  candidate.state = 0x40;
  candidate.stable_frames = mfgunlock::uicandidate::kRequiredStableFrames;
  candidate.rtv_binds_after_clear = 5;
  candidate.reject_reasons = mfgunlock::uicandidate::kAccept;
  candidate.alpha_convention = static_cast<uint32_t>(
      mfgunlock::uialpha::AlphaConvention::kPremultiplied);
  candidate.alpha_draws = 5;
  candidate.alpha_unproven_draws = 0;
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
  fc::g_ui_candidate_path = static_cast<uint32_t>(
      mfgunlock::uisubmission::Path::kColorAndAlpha);
  Check(fc::internal::CandidateOptionsSynchronized(forwarded),
        "accepted Color+Alpha options synchronize matching candidate format");

  fc::g_ui_candidate_ready = false;
  fc::g_ui_candidate_format = 0;
  fc::g_ui_candidate_options_synced = false;
  fc::internal::UpdateUiCandidate(mfgunlock::uisubmission::Path::kAlphaOnly);
  sl::DLSSGOptions alpha_forwarded{};
  Check(fc::internal::BuildUiCompositionOptions(source, alpha_forwarded) &&
            alpha_forwarded.uiBufferFormat == source.uiBufferFormat &&
            alpha_forwarded.enableUserInterfaceRecomposition == sl::Boolean::eTrue &&
            fc::internal::CandidateOptionsSynchronized(alpha_forwarded),
        "accepted UI Alpha options synchronize without a uiBufferFormat override");

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
    if (count == 1 && tags[0].type == sl::kBufferTypeUIColorAndAlpha && tags[0].resource) {
      Check(tags[0].type == sl::kBufferTypeUIColorAndAlpha,
            "injected tag is UI Color+Alpha");
      Check(tags[0].lifecycle == sl::ResourceLifecycle::eOnlyValidNow,
            "injected UI resource is volatile");
      Check(tags[0].extent.width == 2560 && tags[0].extent.height == 1440,
            "injected UI extent matches the output");
      Check(tags[0].resource != nullptr && tags[0].resource->nativeFormat == 29 &&
                tags[0].resource->state == 0x40 &&
                tags[0].resource->native == reinterpret_cast<void*>(0x12340000),
            "injected UI resource carries the validated native metadata");
      if (g_fail_augmented) return sl::Result::eErrorUnsupportedInterface;
    }
    return sl::Result::eOk;
  };
  auto* commands = reinterpret_cast<sl::CommandBuffer*>(0x99);
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            g_forward_calls == 2 && g_seen_count == 1 && g_acquires == 1 &&
            g_releases == 1 && fc::g_ui_candidate_tags_injected.load() == 1,
        "stable candidate forwards native once and submits optional UI once");

  Reset();
  candidate = Candidate();
  candidate.alpha_convention = static_cast<uint32_t>(
      mfgunlock::uialpha::AlphaConvention::kUnknown);
  candidate.alpha_draws = 41;
  candidate.alpha_unproven_draws = 4;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            g_forward_calls == 2 && g_seen_count == 1 && g_acquires == 1 &&
            g_releases == 1 && fc::g_ui_candidate_tags_injected.load() == 1,
        "matching SDR UI preserves the legacy Color+Alpha path with incomplete alpha evidence");

  Reset();
  candidate = Candidate();
  fc::internal::UiViewportState sdr_state{};
  sdr_state.key.store(17, std::memory_order_release);
  sdr_state.options_seen = true; sdr_state.mode = static_cast<uint32_t>(sl::DLSSGMode::eOn);
  Check(fc::internal::TryInjectUiCandidate(
            &sdr_state, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            !sdr_state.automatic_ui_tag_global.load() &&
            sdr_state.automatic_ui_tag_type.load() == 0 &&
            !sdr_state.automatic_ui_clear_pending.load(),
        "legacy SDR Color+Alpha keeps pre-Phase-6 volatile semantics without persistent tag ownership");

  Reset();
  output::Observe(output::FormatKind::kRgb10A2Unorm, 24, true,
                  output::kColorSpaceHdr10Pq, true);
  candidate = Candidate();
  candidate.alpha_convention = static_cast<uint32_t>(
      mfgunlock::uialpha::AlphaConvention::kUnknown);
  candidate.alpha_draws = 41;
  candidate.alpha_unproven_draws = 4;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            g_forward_calls == 1 && g_seen_count == 1 && g_acquires == 0 &&
            fc::g_ui_candidate_tags_injected.load() == 0,
        "HDR10 still fails closed when alpha convention is unproven");

  Reset();
  candidate = Candidate();
  g_fail_augmented = true;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            g_forward_calls == 3 && !fc::g_ui_candidate_runtime_declined.load() &&
            fc::g_ui_candidate_injection_failures.load() == 1 &&
            fc::g_ui_candidate_native_fallbacks.load() == 1 &&
            fc::g_ui_candidate_consecutive_failures.load() == 1 &&
            fc::g_ui_candidate_retry_frames.load() ==
                mfgunlock::uicandidate::kRetryCooldownFrames,
        "one runtime rejection falls back without permanently disabling injection");

  const unsigned int calls_after_failure = g_forward_calls;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            g_forward_calls == calls_after_failure + 1 &&
            fc::g_ui_candidate_injection_failures.load() == 1,
        "retry cooldown forwards native HUD-less without another augmented call");

  fc::g_ui_candidate_retry_frames = 0;
  g_fail_augmented = false;
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
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
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            fc::g_ui_candidate_runtime_declined.load(),
        "repeated failures on one physical resource eventually fail closed");

  Reset();
  candidate = Candidate();
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, nullptr, false, forward) == sl::Result::eOk &&
            g_forward_calls == 1 && g_seen_count == 1 && g_acquires == 0 &&
            fc::g_ui_candidate_missing_command_buffer.load() == 1,
        "missing command buffer preserves the native HUD-less submission");

  Reset();
  output::Observe(output::FormatKind::kRgb10A2Unorm, 24, true,
                  output::kColorSpaceHdr10Pq, true);
  candidate = Candidate();
  Check(fc::internal::TryInjectUiCandidate(
            nullptr, candidate, &hudless_tag, 1, commands, false, forward) == sl::Result::eOk &&
            g_forward_calls == 1 && g_seen_count == 1 && g_acquires == 0 &&
            fc::g_ui_candidate_tags_injected.load() == 0,
        "detected SDR UI is not injected into an HDR10/PQ output domain");

  Reset();
  output::Observe(output::FormatKind::kRgb10A2Unorm, 24, true,
                  output::kColorSpaceHdr10Pq, true);
  fc::g_prepare_ui_alpha_candidate = PrepareAlphaCandidate;
  fc::g_finish_ui_alpha_candidate = FinishAlphaCandidate;
  candidate = Candidate();
  auto alpha_forward = [&](const sl::ResourceTag* tags, uint32_t count,
                           sl::CommandBuffer* copy_commands = nullptr) {
    ++g_forward_calls;
    g_seen_count = count;
    Check(count == 1, "original and helper tags have separate command-list ownership");
    if (copy_commands) {
      Check(copy_commands == reinterpret_cast<sl::CommandBuffer*>(0xCAFE),
            "SL volatile copy follows extraction on its exact native command list");
      Check(tags[0].type == sl::kBufferTypeUIAlpha,
            "HDR automatic UI uses preferred UI Alpha tag");
      Check(tags[0].lifecycle == sl::ResourceLifecycle::eOnlyValidNow,
            "SL owns the clone used until Present");
      Check(tags[0].resource != nullptr &&
                tags[0].resource->native == reinterpret_cast<void*>(0xABCDEF00) &&
                tags[0].resource->nativeFormat == 61,
            "UI Alpha tag carries prepared alpha resource metadata");
    } else {
      Check(tags[0].type == sl::kBufferTypeHUDLessColor,
            "original host tag stays on the original command list");
    }
    return sl::Result::eOk;
  };
  fc::internal::UpdateUiCandidate(fc::internal::CandidateSubmissionPath(candidate));
  fc::g_ui_candidate_options_synced = true;
  fc::internal::UiViewportState alpha_state{};
  alpha_state.key.store(17, std::memory_order_release);
  alpha_state.options_seen = true; alpha_state.mode = static_cast<uint32_t>(sl::DLSSGMode::eOn);
  Check(fc::internal::TryInjectUiCandidate(
            &alpha_state, candidate, &hudless_tag, 1, commands, false, alpha_forward) == sl::Result::eOk &&
            g_forward_calls == 2 && g_seen_count == 1 && g_alpha_prepares == 1 &&
            g_alpha_finishes == 1 && g_acquires == 0 && g_releases == 0 &&
            alpha_state.automatic_ui_tag_global.load() &&
            alpha_state.automatic_ui_tag_type.load() == sl::kBufferTypeUIAlpha &&
            !alpha_state.automatic_ui_clear_pending.load(),
        "HDR UI Alpha path keeps legacy ownership alive across present until it is actually retired");

  fc::internal::RequestGlobalAutomaticUiTagRelease();
  // The standalone alpha_state is not in g_ui_viewports, so request release directly
  // for this unit fixture rather than pretending that Present owns the lifetime.
  alpha_state.automatic_ui_clear_pending.store(true, std::memory_order_release);

  fc::internal::UiViewportState release_state{};
  fc::internal::MarkAutomaticUiTag(
      &release_state, sl::kBufferTypeUIAlpha, false);
  release_state.automatic_ui_clear_pending = true;
  unsigned int release_count = 0;
  auto release_forward = [&](const sl::ResourceTag* tags, uint32_t count) {
    release_count = count;
    Check(count == 2 && tags[1].type == sl::kBufferTypeUIAlpha &&
              tags[1].resource == nullptr,
          "stale legacy automatic UI Alpha is explicitly cleared on the next tag call");
    return sl::Result::eOk;
  };
  Check(fc::internal::ForwardWithAutomaticUiRelease(
            &release_state, &hudless_tag, 1, false, release_forward) == sl::Result::eOk &&
            release_count == 2 &&
            !release_state.automatic_ui_tag_global.load() &&
            release_state.automatic_ui_tag_type.load() == 0,
        "successful legacy clear releases automatic tag ownership");

  fc::internal::MarkAutomaticUiTag(
      &release_state, sl::kBufferTypeUIAlpha, true);
  Check(!release_state.automatic_ui_tag_global.load() &&
            release_state.automatic_ui_tag_type.load() == 0,
        "frame-based automatic UI tag does not create persistent global ownership");

  release_state.key.store(17, std::memory_order_release);
  fc::internal::g_real_set_tag = LegacySetTag;
  fc::internal::MarkAutomaticUiTag(
      &release_state, sl::kBufferTypeUIAlpha, false);
  Check(fc::internal::ReleaseGlobalAutomaticUiTag(&release_state) &&
            g_legacy_clear_calls == 1 &&
            g_legacy_clear_type == sl::kBufferTypeUIAlpha &&
            !release_state.automatic_ui_tag_global.load(),
        "explicit legacy UI Alpha retirement sends a null tag before teardown");

  Reset();
  candidate = Candidate();
  fc::internal::UiViewportState switch_state{};
  switch_state.key.store(17, std::memory_order_release);
  switch_state.options_seen = true; switch_state.mode = static_cast<uint32_t>(sl::DLSSGMode::eOn);
  fc::internal::MarkAutomaticUiTag(
      &switch_state, sl::kBufferTypeUIAlpha, false);
  unsigned int switch_count = 0;
  auto switch_forward = [&](const sl::ResourceTag* tags, uint32_t count) {
    ++switch_count;
    if (switch_count == 1) {
      Check(count == 2 && tags[0].type == sl::kBufferTypeHUDLessColor &&
            tags[1].type == sl::kBufferTypeUIAlpha && tags[1].resource == nullptr,
            "native batch retires only previous helper alpha");
    } else {
      Check(count == 1 && tags[0].type == sl::kBufferTypeUIColorAndAlpha && tags[0].resource,
            "optional SDR color is submitted independently on the host list");
    }
    return sl::Result::eOk;
  };
  Check(fc::internal::TryInjectUiCandidate(
            &switch_state, candidate, &hudless_tag, 1, commands, false, switch_forward) == sl::Result::eOk &&
            switch_count == 2 && !switch_state.automatic_ui_tag_global.load() &&
            switch_state.automatic_ui_tag_type.load() == 0,
        "SDR replacement does not leave persistent automatic UI ownership behind");

  fc::internal::MarkAutomaticUiTag(
      &release_state, sl::kBufferTypeUIAlpha, false);
  unsigned int frame_forward_count = 0;
  auto frame_forward = [&](const sl::ResourceTag*, uint32_t count) {
    frame_forward_count = count;
    return sl::Result::eOk;
  };
  Check(fc::internal::ForwardWithAutomaticUiRelease(
            &release_state, &hudless_tag, 1, true, frame_forward) == sl::Result::eOk &&
            g_legacy_clear_calls == 1 && frame_forward_count == 1 &&
            !release_state.automatic_ui_tag_global.load(),
        "switching to frame-based tags releases legacy global automatic ownership first");
  fc::internal::g_real_set_tag = nullptr;

  // Exercise optional failure isolation through the production branch. The
  // host's volatile batch must not be re-submitted or moved to the helper list.
  for (unsigned failure=0;failure<4;++failure) {
    Reset();
    output::Observe(output::FormatKind::kRgb10A2Unorm,24,true,output::kColorSpaceHdr10Pq,true);
    fc::g_prepare_ui_alpha_candidate=PrepareAlphaCandidate;
    fc::g_finish_ui_alpha_candidate=FinishAlphaCandidate;
    candidate=Candidate();
    g_fail_alpha_finish=failure==2;
    g_bad_alpha_metadata=failure==3;
    unsigned host_calls=0,helper_calls=0,clears=0;
    fc::internal::UiViewportState state{};
    state.options_seen=true;state.mode=static_cast<uint32_t>(sl::DLSSGMode::eOn);
    auto calls=[&](const sl::ResourceTag* tags,uint32_t n,sl::CommandBuffer* own=nullptr){
      Check(n==1,"failure isolation uses one original or optional tag");
      if(tags[0].resource==nullptr){++clears;return sl::Result::eOk;}
      if(tags[0].type==sl::kBufferTypeHUDLessColor){
        Check(!own,"original volatile tag stays on original list");++host_calls;
        return failure==0?sl::Result::eErrorUnsupportedInterface:sl::Result::eOk;
      }
      Check(own!=nullptr,"optional copy uses extraction list");++helper_calls;
      return failure==1?sl::Result::eErrorUnsupportedInterface:sl::Result::eOk;
    };
    auto result=fc::internal::TryInjectUiCandidate(&state,candidate,&hudless_tag,1,commands,true,calls);
    Check(host_calls==1,"never replay the original volatile batch after helper failure");
    Check(result==(failure==0?sl::Result::eErrorUnsupportedInterface:sl::Result::eOk),"optional failure preserves native result");
    Check(!fc::g_ui_candidate_active.load(),"no stale accepted helper after signal/tag/metadata failure");
    Check(g_alpha_finishes==(failure==0?0u:1u),"every prepared lease is finished, no work after native rejection");
    Check(helper_calls==((failure==1||failure==2)?1u:0u),"no optional tag before prerequisites");
    Check(clears==((failure==1||failure==2)?1u:0u),"partially accepted helper is explicitly retired");
  }

  std::printf("PASS UI injection path: %u checks\n", g_checks);
}
