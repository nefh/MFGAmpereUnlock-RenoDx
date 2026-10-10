// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/compatibility_foundation.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace c = mfgunlock::compatibility;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  try {
    // Small executable oracle for the Python corpus test. No DLL loading.
    if (argc > 1 && std::string_view(argv[1]) == "--surface") {
      std::vector<c::ExportSymbol> symbols;
      for (int i = 2; i < argc; ++i) symbols.push_back({argv[i], true});
      std::cout << c::Name(c::ClassifyStreamline(symbols)) << '\n';
      return 0;
    }
    Check(argc == 1, "unexpected test arguments");
    static_assert(!std::is_convertible_v<c::Capability, bool>);
    for (const bool context : {false, true}) {
      for (const bool invariant : {false, true}) {
        Check(c::Evaluate({0, context, invariant}).state == c::CapabilityState::kUnavailable, "zero matches");
        Check(c::Evaluate({2, context, invariant}).state == c::CapabilityState::kAmbiguous, "ambiguous matches");
        Check(c::Evaluate({1, context, invariant}).HasStructuralEvidence() == (context && invariant), "incomplete evidence");
      }
    }
    c::CompatibilityState state;
    Check(state.diagnostics_available && !state.runtime_wiring_present, "unknown-safe default");
    Check(state.runtime.frame_generation.state == c::CapabilityState::kUnknown, "no implicit FG support");
    state.provider_capabilities.retarget_payloads = c::Evaluate({2, true, true});
    Check(state.diagnostics_available, "unsafe feature failure killed diagnostics");
    Check(state.provider_capabilities.preset_code.state == c::CapabilityState::kUnknown, "per-feature isolation");
    Check(state.admission.calling_abi.state == c::CapabilityState::kUnknown, "ABI must be unproven");
    c::PayloadDigest a{80, {}}, b{160, {}};
    a.sha256[0] = 1; b.sha256[0] = 2;
    std::vector<c::PayloadDigest> x{a, a, b}, y{b, a, a};
    Check(c::CompletePayloadMultisetEqual(x, y), "ordering independent");
    y.pop_back();
    Check(!c::CompletePayloadMultisetEqual(x, y), "lost duplicate accepted");
    y = {b, b, a};
    Check(!c::CompletePayloadMultisetEqual(x, y), "multiplicity lost");
    y = x; y[0].sha256[7] ^= 1;
    Check(!c::CompletePayloadMultisetEqual(x, y), "changed payload accepted");
    Check(!c::CompletePayloadMultisetEqual({}, {}), "empty evidence accepted");
    std::vector<c::ExportSymbol> core{{"slInit", true}, {"slShutdown", true}, {"slSetTag", true}, {"slEvaluateFeature", true}};
    auto legacy = core;
    legacy.insert(legacy.end(), {{"slGetFeatureSettings", true}, {"slSetFeatureConstants", true}});
    auto modern = core;
    modern.insert(modern.end(), {{"slGetFeatureFunction", true}, {"slGetFeatureRequirements", true}, {"slGetFeatureVersion", true}});
    Check(c::ClassifyStreamline(core) == c::StreamlineFamily::kUnknownSafe, "insufficient surface");
    Check(c::ClassifyStreamline(legacy) == c::StreamlineFamily::kLegacySurface, "legacy surface");
    Check(c::ClassifyStreamline(modern) == c::StreamlineFamily::kModernSurface, "modern surface");
    auto collision = modern;
    collision.insert(collision.end(), {{"slGetFeatureSettings", true}, {"slSetFeatureConstants", true}});
    Check(c::ClassifyStreamline(collision) == c::StreamlineFamily::kAmbiguous, "ABI collision");
    modern.push_back({"slInit", true});
    Check(c::ClassifyStreamline(modern) == c::StreamlineFamily::kAmbiguous, "duplicate core export");
    legacy[0].own_code = false;
    Check(c::ClassifyStreamline(legacy) == c::StreamlineFamily::kUnknownSafe, "forwarder is not verified local code");
    // Owned identity strings survive the input string lifetime.
    { std::string input = "module.dll"; state.provider.library.path = input; }
    Check(state.provider.library.path == "module.dll", "identity has borrowed storage");
    std::cout << "compatibility foundation PASS (no runtime authorization)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
