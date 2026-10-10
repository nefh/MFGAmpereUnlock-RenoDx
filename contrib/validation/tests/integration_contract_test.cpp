// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>

#include "../../../src/addons/mfgunlock/integration_contract.hpp"

namespace {
unsigned int g_checks = 0;
void Check(bool condition, const char* reason) {
  ++g_checks;
  if (condition) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}
}

int main() {
  namespace ic = mfgunlock::integration;
  Check(static_cast<uint32_t>(ic::ResourceRole::kHudLess) == 3 &&
            static_cast<uint32_t>(ic::ResourceRole::kOutputDisableInterpolation) == 9,
        "legacy resource-role mask values remain ABI-stable");
  Check(static_cast<uint32_t>(ic::ResourceRole::kColorInput) == 10 &&
            static_cast<uint32_t>(ic::ResourceRole::kColorOutput) == 11 &&
            static_cast<uint32_t>(ic::ResourceRole::kExposure) == 12,
        "new resource roles are appended after the legacy diagnostic mask range");
  ic::Reset();
  Check(ic::Read().frame_index == ic::Verdict::kUnknown, "frame contract starts unknown");
  Check(ic::Read().resources == ic::Verdict::kUnknown, "resource contract starts unknown");

  ic::ObserveConstants(42, 7);
  ic::ObservePresentMarker(true, 42);
  ic::ObservePresentMarker(false, 42);
  Check(ic::Read().frame_index == ic::Verdict::kValid, "matching constants and present markers validate");

  ic::ObservePresentMarker(false, 43);
  Check(ic::Read().frame_index == ic::Verdict::kInvalid, "mismatched present marker invalidates frame contract");

  ic::Reset();
  ic::ObserveTag(3, ic::ResourceRole::kDepth, true, true, 1, true, 0, 0, 1280, 720,
                 true, 4, true, 45, 0x1000);
  ic::ObserveTag(3, ic::ResourceRole::kMotionVectors, true, true, 1, true, 0, 0,
                 1280, 720, true, 4, true, 16, 0x2000);
  Check(ic::Read().resources == ic::Verdict::kValid, "depth and motion vectors validate required resource evidence");
  ic::ObserveTag(3, ic::ResourceRole::kColorInput, true, true, 1, true, 0, 0,
                 1280, 720, true, 4, true, 28, 0x3000, true, 42);
  ic::ObserveTag(3, ic::ResourceRole::kColorOutput, true, true, 1, true, 0, 0,
                 1920, 1080, true, 4, true, 28, 0x4000, true, 42);
  ic::ObserveTag(3, ic::ResourceRole::kExposure, true, true, 1, true, 0, 0,
                 1, 1, true, 4, true, 10, 0x5000, true, 42);
  auto tagged = ic::Read();
  const auto& tagged_depth = tagged.resource[static_cast<size_t>(ic::ResourceRole::kDepth)];
  const auto& color_input = tagged.resource[static_cast<size_t>(ic::ResourceRole::kColorInput)];
  const auto& color_output = tagged.resource[static_cast<size_t>(ic::ResourceRole::kColorOutput)];
  const auto& exposure = tagged.resource[static_cast<size_t>(ic::ResourceRole::kExposure)];
  Check(tagged_depth.provenance == ic::ResourceProvenance::kStreamline,
        "Streamline tags retain provenance without taking resource ownership");
  Check(color_input.active && color_output.active && exposure.active,
        "optional color and exposure tags remain observable evidence");
  Check(color_input.frame_known && color_input.frame == 42 &&
            color_output.frame_known && color_output.frame == 42 &&
            exposure.frame_known && exposure.frame == 42,
        "set-tag-for-frame evidence preserves resource frame association");

  ic::ObserveTag(3, ic::ResourceRole::kDepth, true, true, 1, true, 0, 0, 1280, 720,
                 true, 4, true, 45, 0x6000, true, 43);
  auto replaced = ic::Read().resource[static_cast<size_t>(ic::ResourceRole::kDepth)];
  Check(replaced.replacements == 1 && replaced.object == 0x6000 && replaced.frame == 43,
        "resource replacement updates current evidence and increments replacement count");
  ic::ObserveTag(3, ic::ResourceRole::kDepth, false, true, 1, false, 0, 0, 0, 0,
                 false, 0, false, 0, 0);
  auto resource = ic::Read().resource[static_cast<size_t>(ic::ResourceRole::kDepth)];
  Check(resource.seen && !resource.active && resource.clears == 1, "null resource tag is preserved as clear evidence");
  Check(ic::Read().resources == ic::Verdict::kInvalid,
        "clearing a required resource invalidates current resource evidence");

  ic::ObserveHistory(true, false, true, true);
  auto history = ic::Read();
  Check(history.history_reset_known && !history.history_reset &&
            history.rendering_game_frames_known && history.rendering_game_frames,
        "history reset and game-frame evidence remain independent and explicit");
  ic::ObserveHistory(true, true, true, false);
  history = ic::Read();
  Check(history.history_reset && !history.rendering_game_frames,
        "history reset and menu/video evidence update deterministically");

  ic::Reset();
  ic::ObserveSetOptions(5, ic::kDynamicResolutionFlag, 1280, 0, 1280, 720, 2560, 1440,
                        3, true, 0, 10);
  Check(ic::Read().dynamic_resolution == ic::Verdict::kInvalid,
        "half-specified dynamic target is invalid");
  ic::ObserveSetOptions(5, ic::kDynamicResolutionFlag, 0, 0, 1280, 720, 2560, 1440,
                        3, true, 0, 10);
  Check(ic::Read().dynamic_resolution == ic::Verdict::kValid,
        "0/0 dynamic target follows documented default semantics");

  ic::Reset();
  ic::ObserveSetOptions(1, 0, 0, 0, 0, 0, 0, 0, 2, true, 1, 20);
  Check(ic::Read().queue_contract == ic::Verdict::kObserved,
        "no-client-queue mode without fence remains observed, not fabricated valid");
  ic::ObserveState(1, 0, true, true, true, 0x1234, 99);
  Check(ic::Read().queue_contract == ic::Verdict::kObserved,
        "seeing a fence does not prove the presenting queue waited for it");

  ic::Reset();
  ic::ObserveSwapchainInit(0x10, 3, false);
  ic::ObserveSwapchainOutput(24, true, 3, true, 3, true, true);
  ic::ObserveSwapchainPresent(0x10, 33);
  ic::ObserveSwapchainDestroy(0x10, true);
  ic::ObserveSwapchainInit(0x20, 3, true);
  auto swap = ic::Read();
  Check(swap.swapchain == ic::Verdict::kObserved, "swapchain lifecycle is observed without unsupported inference");
  Check(swap.swapchain_resize_count == 2 && swap.swapchain_recreation_count >= 1,
        "resize/recreation accounting is retained");
  Check(swap.swapchain_format_known && swap.swapchain_format == 24 &&
            swap.swapchain_color_space_known && swap.swapchain_color_space == 3 &&
            swap.output_encoding_known && swap.output_encoding == 3 &&
            swap.output_dlssg_supported,
        "swapchain output format, color space and classified encoding remain separate evidence");
  Check(!swap.fullscreen_transition_known && !swap.waitable_object_ownership_known && !swap.iflip_known,
        "fullscreen waitable-object and IFLIP ownership remain unknown without evidence");

  ic::Reset();
  ic::ObserveState(2, ic::kStatusReflexMissing | ic::kStatusHdrUnsupported | (1u << 7),
                   false, false, false, 0, 0);
  auto status = ic::Read();
  Check(status.fail_reflex_missing && status.fail_hdr_unsupported,
        "meaningful DLSS-G status bits stay distinct");
  Check(status.status_unknown_bits == (1u << 7), "unknown DLSS-G status bits are preserved");
  Check(status.frame_index == ic::Verdict::kInvalid, "Reflex failure invalidates frame contract");

  ic::Reset();
  ic::ObserveSetOptions(9, 0, 0, 0, 0, 0, 0, 0, 2, false, 0, 1);
  ic::ObserveConstants(1, 10);
  Check(ic::Read().viewport_ownership == ic::Verdict::kInvalid,
        "cross-viewport ownership mismatch is explicit");

  ic::Reset();
  ic::ObserveNgxResource(ic::ResourceRole::kOutputReal, true, 0x5555, 1920, 1080, 28,
                         true, 100);
  auto ngx = ic::Read();
  Check(ngx.viewport_ownership == ic::Verdict::kUnknown,
        "NGX resource evidence does not invent a Streamline viewport");
  const auto& ngx_output = ngx.resource[static_cast<size_t>(ic::ResourceRole::kOutputReal)];
  Check(ngx_output.active && ngx_output.provenance == ic::ResourceProvenance::kNgx &&
            ngx_output.frame_known && ngx_output.frame == 100,
        "NGX output resource is observable without retaining ownership");
  ic::ObserveNgxResource(ic::ResourceRole::kOutputReal, true, 0x7777, 1920, 1080, 28,
                         true, 101);
  ngx = ic::Read();
  const auto& ngx_replaced = ngx.resource[static_cast<size_t>(ic::ResourceRole::kOutputReal)];
  Check(ngx_replaced.replacements == 1 && ngx_replaced.object == 0x7777 &&
            ngx_replaced.frame == 101,
        "NGX replacement evidence does not retain a stale resource identity");

  ic::ObserveHistory(true, false, true, true);
  ic::ObserveHistory(false, false, false, false);
  Check(!ic::Read().history_reset_known && !ic::Read().rendering_game_frames_known,
        "missing current history evidence must not reuse an older sample");
  Check(ic::InvalidateResource(0x7777), "resource destruction invalidates observed identity");
  const auto invalid = ic::Read().resource[static_cast<size_t>(ic::ResourceRole::kOutputReal)];
  Check(!invalid.active && !invalid.frame_known && !invalid.format_known && invalid.object == 0,
        "destroyed resources leave no usable metadata");
  ic::ObserveNgxResource(ic::ResourceRole::kOutputReal, true, 0x7777, 1920, 1080, 28, true, 102);
  ic::ObserveNgxResource(ic::ResourceRole::kOutputReal, false, 0, 0, 0, 0, false, 0);
  Check(!ic::Read().resource[static_cast<size_t>(ic::ResourceRole::kOutputReal)].active,
        "unavailable NGX parameters clear previous submitted resources");
  ic::ObserveConstants(103, 1);
  ic::ObserveHistory(true, false, true, true);
  ic::ObserveTag(1, ic::ResourceRole::kDepth, true, true, 1, true, 0, 0, 32, 32,
                 true, 4, true, 45, 0x10, true, 103);
  ic::InvalidateFrameEvidence();
  const auto cleared = ic::Read();
  Check(!cleared.constants_frame_known && !cleared.history_reset_known &&
        !cleared.resource[static_cast<size_t>(ic::ResourceRole::kDepth)].active,
        "lifecycle/failure boundary invalidates frame, history and resource together");

  std::cout << "integration_contract_test: " << g_checks << " checks PASS\n";
  return 0;
}
