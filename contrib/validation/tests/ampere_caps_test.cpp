// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/ampere_policy.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace mfgunlock::ampere;

namespace {

unsigned int g_checks = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;

  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}

}  // namespace

int main() {
  Check(IsSupportedAmpere(kNvidiaVendorId, kAmpereArchitecture, 2), "GA102");
  Check(IsSupportedAmpere(kNvidiaVendorId, kAmpereArchitecture, 4), "GA104");
  Check(!IsSupportedAmpere(kNvidiaVendorId, kAmpereArchitecture, 0),
        "GA100/sm80 not admitted");
  Check(!IsSupportedAmpere(kNvidiaVendorId, kAdaArchitecture, 2), "Ada unchanged");
  Check(!IsSupportedAmpere(kNvidiaVendorId, 0x1b0, 2), "Blackwell unchanged");
  Check(!IsSupportedAmpere(0x1002, kAmpereArchitecture, 2), "AMD unchanged");
  Check(!IsSupportedAmpere(0x8086, kAmpereArchitecture, 2), "Intel unchanged");

  const RequirementsEvidence base{true, true, 1, kNgxSuccess, kDlssGFeatureId,
                                  kAdapterUnsupported, kAdaArchitecture};
  Check(CanRelaxRequirements(base), "qualified architecture override");

  auto evidence = base;
  evidence.minimum_architecture = kAmpereArchitecture;
  Check(CanRelaxRequirements(evidence),
        "already-retargeted minimum with unsupported flag");

  evidence.flags = 0;
  Check(!CanRelaxRequirements(evidence), "no change if already supported");
  Check(std::strcmp(RequirementsDecision(evidence), "already-supported") == 0,
        "native supported classification");

  for (uint32_t feature = 0; feature < 40; ++feature) {
    for (uint32_t flags = 0; flags < 64; ++flags) {
      for (uint32_t architecture : {0u, 0x160u, kAmpereArchitecture,
                                    kAdaArchitecture, 0x1b0u, 0xffffffffu}) {
        evidence = base;
        evidence.feature = feature;
        evidence.flags = flags;
        evidence.minimum_architecture = architecture;

        const bool expected =
            feature == kDlssGFeatureId &&
            ((flags == 0 && architecture == kAdaArchitecture) ||
             (flags == kAdapterUnsupported &&
              (architecture == kAdaArchitecture || architecture == kAmpereArchitecture)));

        Check(CanRelaxRequirements(evidence) == expected,
              "complete feature/flag/minimum matrix");
        if (!expected) {
          Check(std::strcmp(RequirementsDecision(evidence), "architecture-override") != 0,
                "diagnostics must agree with admission");
        }
      }
    }
  }

  for (uint32_t result : {0u, 2u, 0xbad00001u, 0xbad00005u, 0xffffffffu}) {
    evidence = base;
    evidence.call_result = result;
    Check(!CanRelaxRequirements(evidence), "preserve original errors");
  }

  evidence = base;
  evidence.enabled = false;
  Check(!CanRelaxRequirements(evidence), "opt-in required");

  evidence = base;
  evidence.ampere_adapter = false;
  Check(!CanRelaxRequirements(evidence), "real adapter required");

  for (unsigned int providers : {0u, 2u, 3u, 8u}) {
    evidence = base;
    evidence.prepared_providers = providers;
    Check(!CanRelaxRequirements(evidence), "unprepared or ambiguous provider");
  }

  for (int mask = 0; mask < 32; ++mask) {
    const bool enabled = (mask & 1) != 0;
    const bool scope = (mask & 2) != 0;
    const bool same_gpu = (mask & 4) != 0;
    const bool provider_ready = (mask & 8) != 0;
    const int result = (mask & 16) ? -1 : 0;
    Check(CanExposeAda(enabled, scope, same_gpu, provider_ready, result,
                       kAmpereArchitecture, 2) == (mask == 15),
          "NVAPI scope and proof matrix");
  }

  Check(!CanExposeAda(true, true, true, true, 0, kAdaArchitecture, 2),
        "no Ada rewrite");
  Check(!CanExposeAda(true, true, true, true, 0, kAmpereArchitecture, 0),
        "no sm80 spoof");

  for (uint32_t bits = 0; bits < 256; ++bits) {
    Check(Hags27Enabled(bits) == ((bits & 1) != 0 && (bits & 2) != 0),
          "HAGS 2.7 decode");
    Check(Hags29Enabled(bits) == ((bits & 3) != 0 && (bits & 4) != 0),
          "HAGS 2.9 decode");
  }
  Check(Hags27Enabled(0xb), "HAGS-enabled sample is recognized");

  std::printf("PASS capability policy: %u checks\n", g_checks);
  return 0;
}
