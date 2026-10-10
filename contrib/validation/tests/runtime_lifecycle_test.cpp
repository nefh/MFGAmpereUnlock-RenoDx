// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>

#include "../../../src/addons/mfgunlock/runtime_lifecycle.hpp"

namespace rt = mfgunlock::runtime;
namespace profiles = mfgunlock::profiles;
namespace architecture = mfgunlock::architecture;

namespace {
unsigned g_checks = 0;
void Check(bool value, const char* reason) {
  ++g_checks;
  if (value) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}
}

int main() {
  rt::Attach(&architecture::kAda);
  auto state = rt::Read();
  Check(state.phase == rt::Phase::kAttached, "attach publishes explicit phase");
  Check(!rt::ProviderReady() && state.fallback_reason == rt::FallbackReason::kProviderMissing,
        "provider is required before capability admission");

  Check(!rt::ObserveDevice(0x1000), "first primary device is not recreation");
  rt::ObserveSwapchainInit(0x2000);
  rt::ObserveProvider(0x3000, 0x1111, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAda);
  rt::ObserveNgx(1, 1, false);
  state = rt::Read();
  Check(state.phase == rt::Phase::kFeatureReady && rt::ProviderReady(),
        "exact native provider plus tracked feature reaches feature-ready");
  Check(state.allowed_capability_bits == mfgunlock::features::kNativeSm89BackendBits,
        "allowed capabilities come from selected backend");

  rt::ObserveSwapchainDestroy(0x2000);
  rt::ObserveSwapchainInit(0x2000);
  Check(rt::Read().swapchain_generation == 2,
        "same-address swapchain recreation advances generation");
  rt::ObserveSwapchainDestroy(0x2000);
  rt::ObserveSwapchainInit(0x2001);
  Check(rt::Read().swapchain_generation == 3,
        "new-address swapchain recreation advances generation");

  Check(rt::ObserveDevice(0x1001), "new selected primary device is recreation");
  Check(rt::Read().device_generation == 2, "device recreation advances generation");

  rt::ObserveProvider(0x3000, 0x1111, 1, profiles::Qualification::kObserveOnly, false,
                      &architecture::kAda);
  Check(rt::ProviderReady() && !rt::Read().provider_exact_qualified,
        "native provider remains eligible without private inventory evidence");
  rt::ObserveProvider(0x3000, 0x1111, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAda);
  Check(rt::ProviderReady(), "same exact provider can recover without generation change");

  rt::ObserveProvider(0x3001, 0x2222, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAda);
  state = rt::Read();
  Check(state.restart_required && !rt::ProviderReady() &&
            state.fallback_reason == rt::FallbackReason::kProviderChanged,
        "provider reload after a live generation fails closed and requires restart");

  rt::Attach(&architecture::kAmpere);
  rt::ObserveProvider(0x3500, 0x2525, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAmpere);
  Check(!rt::ProviderReady() && rt::Read().allowed_capability_bits == 0,
        "retarget backend is not admitted until provider preparation is committed");
  rt::ObserveProvider(0x3500, 0x2525, 1, profiles::Qualification::kPatchable, true,
                      &architecture::kAmpere);
  Check(rt::ProviderReady(), "patchable exact retarget provider is admitted");
  rt::ObserveProvider(0x3500, 0x2525, 0, profiles::Qualification::kPatchable, false,
                      &architecture::kAmpere);
  Check(rt::ProviderReady() && !rt::Read().provider_exact_qualified,
        "committed structural trial is ready without a registry identity");
  rt::ObserveNgx(1, 1, false);
  Check(rt::Read().phase == rt::Phase::kFeatureReady,
        "unknown registry provider reaches feature-ready after actual creation");

  rt::Attach(&architecture::kAmpere);
  rt::ObserveProviderAmbiguous();
  Check(rt::Read().phase == rt::Phase::kDegraded && !rt::ProviderReady(),
        "ambiguous providers never authorize a retarget backend");

  rt::Attach(&architecture::kTuring);
  rt::ObserveProvider(0x4000, 0x3333, 1, profiles::Qualification::kPatchable, true,
                      &architecture::kTuring);
  rt::ObserveNgx(7, 1, true);
  Check(rt::Read().fallback_reason == rt::FallbackReason::kNgxTrackingLost &&
            !rt::ProviderReady(),
        "uncertain NGX tracking removes runtime capabilities");

  rt::Attach(&architecture::kAda);
  rt::ObserveProvider(0x5000, 0x4444, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAda);
  rt::ObserveProviderMissing();
  Check(rt::Read().restart_required && !rt::ProviderReady(),
        "provider disappearance after qualification is restart-required");

  rt::Attach(&architecture::kAda);
  rt::ObserveProvider(0x5100, 0x5555, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAda);
  rt::ObserveProviderMissing(true);
  Check(!rt::Read().restart_required && !rt::Read().provider_known && !rt::ProviderReady(),
        "verified clean unload invalidates identity without retaining stale pointers");
  rt::ObserveProvider(0x5100, 0x5556, 1, profiles::Qualification::kQualified, true,
                      &architecture::kAda);
  Check(rt::ProviderReady() && rt::Read().provider_generation == 3,
        "clean reload at the same base creates a fresh provider generation");
  rt::ObserveNgx(2, 1, false);
  rt::ObserveProviderMissing(true);
  Check(rt::Read().restart_required && !rt::ProviderReady(),
        "active NGX feature prevents clean-unload recovery");

  rt::BeginShutdown();
  state = rt::Read();
  Check(state.phase == rt::Phase::kShuttingDown && state.live_unload_safe == 0,
        "shutdown is explicit and live unload is not advertised safe");

  rt::Attach(&architecture::kAda);
  Check(rt::Read().revision == 1 && !rt::Read().restart_required,
        "repeated attach resets process-lifetime state deterministically");

  rt::ObserveDevice(0x100);
  Check(!rt::ObserveDeviceDestroy(0x200), "secondary-device destruction does not change primary state");
  const auto generation = rt::Read().device_generation;
  Check(rt::ObserveDeviceDestroy(0x100) && !rt::Read().device_known,
        "primary-device destruction is explicit");
  Check(rt::ObserveDevice(0x100) && rt::Read().device_generation == generation + 1,
        "destroy and recreate at the same address is a new device generation");
  rt::ObserveProvider(0x500, 0x55, 1, profiles::Qualification::kQualified, true, &architecture::kAda);
  rt::ObserveNgx(1, 1, false);
  rt::ObserveNgx(1, 0, false);
  Check(rt::Read().phase != rt::Phase::kFeatureReady, "last feature release demotes feature-ready");
  rt::ObserveNgx(1, 1, true);
  rt::ObserveProvider(0x500, 0x55, 1, profiles::Qualification::kQualified, true, &architecture::kAda);
  Check(!rt::ProviderReady(), "provider polling must not clear uncertain NGX ownership");
  rt::BeginShutdown();
  rt::ObserveProvider(0x500, 0x55, 1, profiles::Qualification::kQualified, true, &architecture::kAda);
  Check(rt::Read().phase == rt::Phase::kShuttingDown && !rt::ProviderReady(),
        "diagnostics/provider polling cannot re-admit a shutting-down runtime");

  rt::ObserveDevice(0x999);
  rt::ObserveSwapchainInit(0x888);
  rt::ObserveSwapchainDestroy(0x888);
  rt::ObserveDeviceDestroy(0x999);
  Check(rt::Read().phase == rt::Phase::kShuttingDown,
        "late device/swapchain callbacks do not revive shutdown");
  // Observed clean unload is a generation boundary even when a replacement
  // retains the same base/metadata token. This is not a file-SHA or ABA detector.
  rt::Attach(&architecture::kAda);
  for (unsigned int iteration = 0; iteration < 64; ++iteration) {
    rt::ObserveProvider(0x7000, 0x1234, 0, profiles::Qualification::kUnknown, false,
                        &architecture::kAda);
    Check(rt::ProviderReady() && !rt::Read().restart_required && !rt::Read().provider_exact_qualified,
          "unknown native generation stays eligible without claiming a registry match");
    const auto before = rt::Read().provider_generation;
    rt::ObserveProvider(0x7000, 0x1234, 0, profiles::Qualification::kUnknown, false,
                        &architecture::kAda);
    Check(rt::Read().provider_generation == before, "repeated observation is idempotent");
    rt::ObserveProviderMissing(true);
    Check(!rt::Read().provider_known && !rt::Read().restart_required,
          "repeated clean unload clears generation state");
  }
  std::cout << "runtime_lifecycle_test: " << g_checks << " checks PASS\n";
  return 0;
}
