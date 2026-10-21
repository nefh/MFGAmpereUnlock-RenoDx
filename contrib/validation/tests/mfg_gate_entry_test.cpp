// SPDX-License-Identifier: MIT
// Exact production addon gate/control flow plus real provider resolver/writer.
// Only mapped OS memory and unrelated runtime readiness are API doubles.
#include "mfg_gate_fixture.hpp"
#include <iostream>
#include <sstream>
using mfgunlock::g_enabled;
std::atomic_bool g_gate_patched{false}, g_midpoint_patched{false}, g_blackwell_temporal_patched{false};
std::vector<mfgunlock::provider::internal::BytePatch> g_gate_sites;
std::vector<HMODULE> g_gate_modules;
std::atomic<unsigned> g_mfg_gate_ceiling{0}, g_mfg_gate_candidates{0};
std::atomic<const char*> g_mfg_gate_status{"NOT_ATTEMPTED"};
namespace mfgunlock::runtime {
bool ready = true;
bool ProviderReady() { return ready; }
}
bool HasTemporalPatch(HMODULE) { return false; }  // Regression: no optional kernel descriptor.
#include "addon_mfg_gate_seam.hpp"
unsigned checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
int main() { try {
  using namespace mfgunlock;
  mfg_gate_fixture::Reset();
  architecture::Configure(Architecture::kAda);
  mfg_gate_fixture::Image image(3);
  const auto original = image.bytes;
  Check(!ProductionMultiFrameReady(), "native Ada MFG not ready before host gates");
  PatchArchGatesInModule(image.Module());
  Check(g_gate_patched && g_gate_sites.size() == 2 && g_mfg_gate_ceiling == 3 &&
        !std::strcmp(g_mfg_gate_status, "APPLIED"), "unlisted image reaches actual addon commit, no registry gate");
  Check(ProductionMultiFrameReady(), "structurally patched native Ada reaches production readiness");
  const auto writes = provider_mock::protection_calls;
  PatchArchGatesInModule(image.Module());
  Check(provider_mock::protection_calls == writes, "repeated attachment does not patch twice");
  runtime::ready = false;
  Check(!ProductionMultiFrameReady(), "provider uncertainty still suppresses dependent readiness");
  runtime::ready = true;
  g_enabled = false;
  Check(!ProductionMultiFrameReady(), "OFF remains OFF");
  g_enabled = true;
  provider_mock::fail_protection_call = provider_mock::protection_calls + 1;
  RestoreDlssgArchGate();
  Check(!g_gate_patched && g_mfg_gate_ceiling == 0 && !g_gate_sites.empty(),
        "incomplete restore retains undo but revokes readiness and ceiling");
  Check(!std::strcmp(g_mfg_gate_status, "ROLLBACK_INCOMPLETE"),
        "incomplete restore must not keep UI status APPLIED");
  provider_mock::fail_protection_call = 0;
  RestoreDlssgArchGate();
  Check(g_gate_sites.empty() && image.bytes == original && !ProductionMultiFrameReady(),
        "retry restores exact bytes");
  PatchArchGatesInModule(image.Module());
  Check(g_gate_sites.empty() && image.bytes == original &&
        !std::strcmp(g_mfg_gate_status, "MEMORY_TRANSACTION_INCOMPLETE"),
        "comparison retry cannot clear rollback failures owned by other transactions");
  provider::Restore();
  Check(!provider::internal::g_registry_failed.load(),
        "full restoration releases the transaction failure after checking all owners");
  provider::g_create_seen = true;
  PatchArchGatesInModule(image.Module());
  Check(g_gate_sites.empty() && !std::strcmp(g_mfg_gate_status, "STARTUP_MISSED"),
        "no guessed live patch after Create");
  provider::g_create_seen = false;
  image.bytes[image.cap_branch] = 0x74;
  PatchArchGatesInModule(image.Module());
  Check(!g_gate_patched && g_gate_sites.empty(), "unresolved current contract never advertises ready");
  image.bytes = original; image.Bind();
  PatchArchGatesInModule(image.Module());
  Check(g_gate_patched, "prior unresolved same address does not suppress a fresh structural attempt");
  RestoreDlssgArchGate();
  // Prepared-image fixture: the provider transformation is tested elsewhere.
  // Exercise the actual addon entry and readiness with optional temporal absent.
  for (auto arch : {Architecture::kAmpere, Architecture::kTuring}) {
    mfg_gate_fixture::Reset(); image.Bind();
    architecture::Configure(arch);
    provider::internal::Provider prepared{};
    prepared.module = image.Module(); prepared.architecture = arch;
    prepared.ready = true; prepared.qualification = profiles::Qualification::kPatchable;
    provider::internal::ReadIdentity(prepared.module, prepared.identity);
    prepared.patches.push_back({image.bytes.data()+0x7f00,0,0});
    provider::internal::g_providers = {prepared};
    Check(provider::PreparedProviderCount() == 1, "fixture has one current prepared provider");
    Check(!HasTemporalPatch(image.Module()) && !g_midpoint_patched && !g_blackwell_temporal_patched,
          "optional temporal transform is absent");
    PatchArchGatesInModule(image.Module());
    Check(g_gate_patched && ProductionMultiFrameReady(),
          "missing optional temporal transform does not veto MFG gates or menu capability");
    RestoreDlssgArchGate();
    provider::internal::g_providers.clear();
  }
  mfg_gate_fixture::Reset(); image.Bind();
  architecture::Configure(Architecture::kUnknown);
  PatchArchGatesInModule(image.Module());
  Check(!g_gate_patched && g_gate_sites.empty() && image.bytes == original &&
        !std::strcmp(g_mfg_gate_status, "NATIVE_ARCH_VALUE_MISSING"),
        "byte rewrite does not invent its numeric architecture operand");
  RestoreDlssgArchGate();
  std::cout << "PASS: " << checks << " production MG1 entry/readiness checks\n";
  return 0;
} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; } }
