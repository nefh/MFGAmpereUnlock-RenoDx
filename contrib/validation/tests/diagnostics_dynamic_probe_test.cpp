// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgdiagnostics/dynamic_probe.hpp"
#include "../../../src/addons/mfgdiagnostics/interposer_discovery.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace dp = mfgdiagnostics::dynamicprobe;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (value) return;
  std::fprintf(stderr, "FAIL diagnostics Dynamic: %s\n", message);
  std::exit(1);
}
struct Call {
  bool probe;
  uint32_t viewport, version, initial_capability, options_version, color_width;
  const void* next;
  const void* options;
  const void* options_next;
  const void* callback;
};
std::vector<Call> calls;
std::vector<sl::Result> results;
sl::DLSSGState* game_state = nullptr;
int capability = -1;
uint32_t native_presented = 7, probe_presented = 5;
unsigned leases = 0;
std::vector<unsigned> traces;
std::function<void(bool)> inside;

void Reset() {
  calls.clear();
  results.clear();
  capability = -1;
  native_presented = 7;
  probe_presented = 5;
  leases = 0;
  traces.clear();
  inside = {};
}

sl::Result GetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                    const sl::DLSSGOptions* options) {
  const bool probe = &state != game_state;
  calls.push_back({probe, static_cast<uint32_t>(viewport), state.structVersion,
      state.structVersion == 4 ? static_cast<uint32_t>(state.bIsDynamicMFGSupported) : UINT32_MAX,
      options ? options->structVersion : 0, options ? options->colorWidth : 0,
      state.next, options, options ? options->next : nullptr,
      options ? options->onErrorCallback : nullptr});
  const auto result = calls.size() <= results.size() ? results[calls.size() - 1] : sl::Result::eOk;
  if (dp::KnownState(state)) {
    state.numFramesActuallyPresented = probe ? probe_presented : native_presented;
    if (state.structVersion == 4 && capability >= 0)
      state.bIsDynamicMFGSupported = static_cast<sl::Boolean>(capability);
  }
  if (inside) inside(probe);
  return result;
}
sl::Result OtherGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                         const sl::DLSSGOptions* options) {
  return GetState(viewport, state, options);
}
sl::Result Run(dp::ManualProbe& probe, sl::DLSSGState& state,
               const sl::DLSSGOptions* options = nullptr, uint32_t viewport = 17,
               bool current = true, bool acquire = true, dp::GetState real = GetState) {
  game_state = &state;
  return probe.OnGetState(real, sl::ViewportHandle(viewport), state, options,
      [&] { return current; }, [&] { ++leases; return acquire; },
      [&](bool begin, sl::Result) { traces.push_back(begin ? calls.size() : calls.size() + 1000); });
}
}  // namespace

int main() {
  const auto renamed = reinterpret_cast<HMODULE>(0x1500);
  const auto legacy_module = reinterpret_cast<HMODULE>(0x2500);
  const auto forwarder = reinterpret_cast<HMODULE>(0x3500);
  const auto resolver = reinterpret_cast<FARPROC>(&GetState);
  mock::exports[{renamed, "slGetFeatureFunction"}] = resolver;
  mock::exports[{legacy_module, "slSetTag"}] = resolver;
  mock::exports[{forwarder, "slGetFeatureFunction"}] = resolver;
  mock::allocation[reinterpret_cast<const void*>(resolver)] = renamed;
  Check(mfgdiagnostics::interposer::Modern(renamed),
        "companion discovers own modern resolver without a conventional DLL filename");
  Check(!mfgdiagnostics::interposer::Modern(legacy_module) &&
        !mfgdiagnostics::interposer::Modern(forwarder) &&
        !mfgdiagnostics::interposer::Modern(nullptr),
        "legacy tag and foreign forwarded code never establish the modern call ABI");
  mfgdiagnostics::interposer::InventoryRequest inventory;
  Check(inventory.Take(), "already loaded renamed modules get one initial inventory");
  bool extra_inventory = false;
  for (unsigned frame = 0; frame < 1000; ++frame) extra_inventory |= inventory.Take();
  Check(!extra_inventory, "absence of an interposer never causes a full inventory every frame");
  inventory.Loaded();
  Check(inventory.Take() && !inventory.Take(), "observed load rearms exactly one inventory outside loader callbacks");

  // Both old and current caller states lead to a real independent v4 call.
  // Neither the production helper nor its addon call site has a graphics-API
  // prerequisite; the callable typed getter is the only query entry point.
  for (uint32_t version = 1; version <= 4; ++version) {
    for (uint32_t option_version = 1; option_version <= 5; ++option_version) {
      for (int report : {-1, 0, 1}) {
        Reset();
        dp::ManualProbe probe;
        sl::DLSSGState state{};
        state.structVersion = version;
        state.next = reinterpret_cast<void*>(0x1234);
        sl::DLSSGOptions options{};
        options.structVersion = option_version;
        options.next = reinterpret_cast<void*>(0x5678);
        options.onErrorCallback = reinterpret_cast<void*>(0x9876);
        options.colorWidth = 3840;
        options.dynamicTargetFrameRate = 144.0f;
        capability = report;
        probe.Arm();
        const auto result = Run(probe, state, &options);
        const auto seen = probe.Read();
        Check(result == sl::Result::eOk && calls.size() == 2 && leases == 1 &&
              !calls[0].probe && calls[1].probe && calls[1].version == 4 &&
              calls[0].viewport == 17 && calls[1].viewport == 17,
              "manual action executes original and owned v4 getter on the current viewport");
        Check(calls[0].options == &options && calls[0].next == state.next &&
              calls[1].next == nullptr && calls[1].options_next == nullptr &&
              calls[1].callback == nullptr && calls[1].options_version == option_version &&
              calls[1].color_width == 3840 && options.next == reinterpret_cast<void*>(0x5678) &&
              options.onErrorCallback == reinterpret_cast<void*>(0x9876),
              "owned query copies bounded options and isolates caller extension/callback targets");
        Check(calls[1].initial_capability == UINT32_MAX && seen.attempted && seen.from_probe &&
              seen.seen == (report >= 0) && seen.supported == (report == 1) &&
              seen.result == 0 && seen.viewport == 17 && state.numFramesActuallyPresented == 12,
              "false true and unwritten capability stay distinct and consumed presentations return to game");
        Check(traces == std::vector<unsigned>{1, 1002} && !probe.Armed(),
              "BEGIN precedes actual extra query and one-shot is consumed exactly once");
        capability = 0;
        for (unsigned i = 0; i < 64; ++i) Run(probe, state, &options);
        Check(calls.size() == 66 && leases == 1 && traces.size() == 2 &&
              probe.Read().seen == (report >= 0) && probe.Read().supported == (report == 1),
              "normal polling performs no more probes or loader leases and retains explicit probe result");
      }
    }
  }

  Reset();
  dp::ManualProbe probe;
  sl::DLSSGState state{};
  state.structVersion = 2;
  Run(probe, state);
  Check(calls.size() == 1 && leases == 0 && !probe.Read().attempted,
        "unarmed observer only forwards the native call");
  probe.Arm();
  Check(Run(probe, state, nullptr, 17, true, true, nullptr) == sl::Result::eErrorNotInitialized &&
        calls.size() == 1 && probe.Armed(), "missing getter never fakes an attempted query");
  Run(probe, state, nullptr, 17, false);
  Check(calls.size() == 2 && leases == 0 && probe.Armed(),
        "changed getter binding preserves native forwarding without a stale extra query");
  Run(probe, state, nullptr, 17, true, false);
  Check(calls.size() == 3 && leases == 1 && probe.Armed(),
        "unavailable current module lifetime leaves manual probe pending");
  Run(probe, state);
  Check(calls.size() == 5 && probe.Read().attempted && !probe.Armed(),
        "later valid native lifetime permits the actual query");

  Reset(); probe.Invalidate(); probe.Arm();
  results = {sl::Result::eErrorNotInitialized, sl::Result::eOk};
  Check(Run(probe, state) == sl::Result::eErrorNotInitialized &&
        state.numFramesActuallyPresented == 7 && calls.size() == 2,
        "successful probe never fabricates success for rejected original GetState");
  Run(probe, state, nullptr, 18);
  Check(state.numFramesActuallyPresented == 7, "presentation carry cannot cross viewports");
  Run(probe, state, nullptr, 17, true, true, OtherGetState);
  Check(state.numFramesActuallyPresented == 7, "presentation carry cannot cross getter bindings");
  Run(probe, state);
  Check(state.numFramesActuallyPresented == 12, "next successful matching native call receives consumed count");
  Run(probe, state);
  Check(state.numFramesActuallyPresented == 7, "consumed presentation carry drains exactly once");

  Reset(); probe.Invalidate(); probe.Arm();
  results = {sl::Result::eOk, sl::Result::eErrorFeatureNotSupported};
  capability = 1;
  Check(Run(probe, state) == sl::Result::eOk && state.numFramesActuallyPresented == 7 &&
        !probe.Read().seen && probe.Read().result == static_cast<uint32_t>(sl::Result::eErrorFeatureNotSupported),
        "actual failed probe result stays diagnostic and untrusted failure counts are not added");

  Reset(); probe.Invalidate(); probe.Arm();
  state.structVersion = 99;
  state.numFramesActuallyPresented = 111;
  Run(probe, state);
  Check(calls.size() == 2 && calls[1].version == 4 && state.numFramesActuallyPresented == 111,
        "unknown native State stays opaque while separate owned typed query executes");
  state.structVersion = 2;
  Run(probe, state);
  Check(state.numFramesActuallyPresented == 12, "opaque caller defers consumed count to known native state");

  Reset(); probe.Invalidate(); probe.Arm();
  inside = [&](bool extra) { if (!extra) probe.Invalidate(); };
  Run(probe, state);
  Check(calls.size() == 1 && !probe.Armed() && !probe.Read().attempted,
        "device or swapchain generation change during original call cancels extra query");
  Reset(); probe.Arm(); capability = 1;
  inside = [&](bool extra) { if (extra) probe.Invalidate(); };
  Run(probe, state);
  Check(calls.size() == 2 && !probe.Read().seen && !probe.Read().attempted &&
        state.numFramesActuallyPresented == 12,
        "generation change during probe discards stale metadata but preserves this call's consumed count");

  Reset(); probe.Invalidate(); probe.Arm();
  results = {sl::Result::eErrorNotInitialized, sl::Result::eOk};
  Run(probe, state);
  probe.Invalidate();
  Run(probe, state);
  Check(state.numFramesActuallyPresented == 7, "pending count from destroyed generation is not replayed");

  Reset(); probe.Invalidate(); probe.Arm(); capability = 1;
  inside = [&](bool extra) { if (extra) probe.Arm(); };
  Run(probe, state);
  Check(probe.Armed() && !probe.Read().attempted && state.numFramesActuallyPresented == 12,
        "new manual request is not overwritten by an older in-flight query result");
  inside = {};
  Run(probe, state);
  Check(calls.size() == 4 && !probe.Armed() && probe.Read().seen,
        "newly armed request executes its own native query");

  Reset(); probe.Invalidate(); probe.Arm();
  native_presented = UINT32_MAX - 2;
  Run(probe, state);
  Check(state.numFramesActuallyPresented == UINT32_MAX, "presentation sum saturates instead of wrapping");

  Reset(); probe.Invalidate();
  for (uint32_t viewport = 0; viewport < 8; ++viewport) {
    results.push_back(sl::Result::eErrorNotInitialized);
    results.push_back(sl::Result::eOk);
    probe.Arm();
    Run(probe, state, nullptr, viewport);
  }
  probe.Arm();
  results.push_back(sl::Result::eErrorNotInitialized);
  Run(probe, state, nullptr, 8);
  Check(calls.size() == 17 && probe.Armed(),
        "full pending storage never permits another consuming query that would lose counts");
  Run(probe, state, nullptr, 0);
  Check(calls.size() == 19 && !probe.Armed() && state.numFramesActuallyPresented == 17,
        "draining one viewport frees storage and preserves both earlier and current probe counts");

  // Physically short allocations catch accidental field reads beyond the
  // public v1 tail under ASan, rather than merely relabelling a full structure.
  Reset(); probe.Invalidate(); probe.Arm();
  sl::DLSSGOptions source{};
  source.structVersion = 1;
  source.colorWidth = 2560;
  constexpr size_t option_bytes = offsetof(sl::DLSSGOptions, bReserved15);
  auto short_options = std::make_unique<unsigned char[]>(option_bytes);
  std::memcpy(short_options.get(), &source, option_bytes);
  sl::DLSSGState source_state{};
  source_state.structVersion = 1;
  constexpr size_t state_bytes = offsetof(sl::DLSSGState, numFramesToGenerateMax);
  auto short_state = std::make_unique<unsigned char[]>(state_bytes);
  std::memcpy(short_state.get(), &source_state, state_bytes);
  auto& legacy = *reinterpret_cast<sl::DLSSGState*>(short_state.get());
  Run(probe, legacy, reinterpret_cast<sl::DLSSGOptions*>(short_options.get()));
  Check(calls.size() == 2 && calls[1].color_width == 2560 &&
        legacy.numFramesActuallyPresented == 12 && !probe.Read().seen,
        "physically short v1 State/Options stay in bounds through production query path");

  std::printf("PASS diagnostics Dynamic probe: %u checks\n", checks);
}
