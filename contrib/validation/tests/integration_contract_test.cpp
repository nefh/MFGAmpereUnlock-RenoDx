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
  ic::ObserveTag(3, ic::ResourceRole::kDepth, false, true, 1, false, 0, 0, 0, 0,
                 false, 0, false, 0, 0);
  auto resource = ic::Read().resource[static_cast<size_t>(ic::ResourceRole::kDepth)];
  Check(resource.seen && !resource.active && resource.clears == 1, "null resource tag is preserved as clear evidence");

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
  Check(ic::Read().queue_contract == ic::Verdict::kValid,
        "queue mode with completion fence evidence validates");

  ic::Reset();
  ic::ObserveSwapchainInit(0x10, 3, false);
  ic::ObserveSwapchainPresent(0x10, 33);
  ic::ObserveSwapchainDestroy(0x10, true);
  ic::ObserveSwapchainInit(0x20, 3, true);
  auto swap = ic::Read();
  Check(swap.swapchain == ic::Verdict::kObserved, "swapchain lifecycle is observed without unsupported inference");
  Check(swap.swapchain_resize_count == 2 && swap.swapchain_recreation_count >= 1,
        "resize/recreation accounting is retained");
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
  ic::ObserveNgxResource(ic::ResourceRole::kOutputReal, true, 0x5555, 1920, 1080, 28);
  auto ngx = ic::Read();
  Check(ngx.viewport_ownership == ic::Verdict::kUnknown,
        "NGX resource evidence does not invent a Streamline viewport");
  Check(ngx.resource[static_cast<size_t>(ic::ResourceRole::kOutputReal)].active,
        "NGX output resource is observable without retaining ownership");

  std::cout << "integration_contract_test: " << g_checks << " checks PASS\n";
  return 0;
}
