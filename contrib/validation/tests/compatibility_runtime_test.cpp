// SPDX-License-Identifier: MIT
#include <cassert>
#include <array>

#include "../../../src/addons/mfgunlock/compatibility_runtime.hpp"

using namespace mfgunlock::compatibility;

int main() {
  constexpr std::array<ExportSymbol, 7> modern{
      ExportSymbol{"slInit", true}, {"slShutdown", true}, {"slSetTag", true},
      {"slEvaluateFeature", true}, {"slGetFeatureFunction", true},
      {"slGetFeatureRequirements", true}, {"slGetFeatureVersion", true}};
  constexpr std::array<ExportSymbol, 6> legacy{
      ExportSymbol{"slInit", true}, {"slShutdown", true}, {"slSetTag", true},
      {"slEvaluateFeature", true}, {"slGetFeatureSettings", true},
      {"slSetFeatureConstants", true}};

  const auto known = AssessInterposer(modern, true, true);
  assert(known.surface == StreamlineFamily::kModernSurface);
  assert(known.typed_abi == TypedInterposerAbi::kModern2 && known.admission.Available());
  // Versions are not inputs. Each callable operation is independently eligible.
  assert(AssessInterposer(legacy, true, true).typed_abi == TypedInterposerAbi::kLegacy1);
  assert(AssessInterposer(modern, false, true).admission.Available());  // No common-name dependency.
  assert(!AssessInterposer(modern, true, false).admission.Available());  // No call through a disappeared image.
  auto forwarded = modern;
  forwarded[0].own_code = false;
  assert(AssessInterposer(forwarded, true, true).admission.Available());
  forwarded[4].own_code = false;
  assert(!AssessInterposer(forwarded, true, true).admission.Available());
  const std::array<ExportSymbol, 1> resolver_only{{{"slGetFeatureFunction", true}}};
  assert(AssessInterposer(resolver_only, false, true).admission.Available());
  auto mixed = std::array<ExportSymbol, modern.size() + 1>{};
  for (size_t i = 0; i < modern.size(); ++i) mixed[i] = modern[i];
  mixed.back() = {"slSetFeatureConstants", true};
  assert(AssessInterposer(mixed, true, true).admission.Available());

  PluginGatewayEvidence plugin{};
  plugin.named_dlssg = plugin.own_code_gateway = plugin.current_image = true;
  plugin.set_options = plugin.get_state = true;
  assert(PluginFunctions(plugin, TypedInterposerAbi::kModern2).Available());
  plugin.get_state = false;
  assert(PluginFunctions(plugin, TypedInterposerAbi::kModern2).Available());
  assert(PluginFunctions(plugin, TypedInterposerAbi::kNone).Available());
  plugin.set_options = false;
  assert(!PluginFunctions(plugin, TypedInterposerAbi::kModern2).Available());
  plugin.get_state = true;
  assert(PluginFunctions(plugin, TypedInterposerAbi::kNone).Available());
  plugin.own_code_gateway = false;
  assert(!GatewayLookup(plugin).Available());  // structural match is not enough
  plugin.own_code_gateway = true;
  plugin.on_load = plugin.on_startup = true;
  plugin.get_state = false;
  assert(PluginFunctions(plugin, TypedInterposerAbi::kLegacy1).state == FeatureState::kDegraded);

  DynamicEvidence dynamic{};
  dynamic.live_set_options = dynamic.live_get_state = true;
  dynamic.options_v5 = dynamic.state_v4 = dynamic.hook_entries_current = true;
  assert(DynamicProbe(dynamic).Available());
  assert(DynamicStack(dynamic).Available());
  // Every combination includes Vulkan, unknown architecture/provider, missing
  // optional hooks and a recorded false support bit. All still get real calls.
  for (unsigned mask = 0; mask < 1024; ++mask) {
    dynamic.architecture_supports = (mask & 1) != 0;
    dynamic.target_ready = (mask & 2) != 0;
    dynamic.provider_qualified = (mask & 4) != 0;
    dynamic.typed_modern_interposer = (mask & 8) != 0;
    dynamic.typed_modern_plugin = (mask & 16) != 0;
    dynamic.hook_entries_current = (mask & 32) != 0;
    dynamic.ceiling_observed = (mask & 64) != 0;
    dynamic.d3d12 = (mask & 128) != 0;
    dynamic.api_support_observed = (mask & 256) != 0;
    dynamic.api_supports_dynamic = (mask & 512) != 0;
    assert(DynamicStack(dynamic).Available());
    assert(DynamicProbe(dynamic).Available());
  }
  dynamic.live_get_state = false;
  assert(DynamicStack(dynamic).Available() && !DynamicProbe(dynamic).Available());
  dynamic.live_get_state = true;
  dynamic.live_set_options = false;
  assert(!DynamicStack(dynamic).Available() && DynamicProbe(dynamic).Available());
  dynamic.live_set_options = true;
  dynamic.options_v5 = false;
  assert(!DynamicStack(dynamic).Available() && DynamicProbe(dynamic).Available());
  dynamic.options_v5 = true;
  dynamic.state_v4 = false;
  assert(DynamicStack(dynamic).Available() && !DynamicProbe(dynamic).Available());
  // Independent diagnostic and admission evidence is never overwritten by a
  // failed provider decision; the decision object has no global support bit.
  assert(known.admission.Available());
  assert(LateAttach(true, false, false).state == FeatureState::kUnknown);
  assert(LateAttach(true, false, true).state == FeatureState::kDegraded);
  assert(LateAttach(false, false, false).state == FeatureState::kUnavailable);
  assert(LateAttach(true, true, false).state == FeatureState::kUnknown);
  assert(LateAttach(true, true, true).Available());

  ModuleGeneration generation;
  assert(generation.Observe(0x1000, 1, 4096));
  assert(!generation.Observe(0x1000, 1, 4096));
  assert(generation.Observe(0, 0, 0));
  assert(!generation.live && generation.epoch == 2);
  assert(generation.Observe(0x1000, 2, 4096));
  assert(generation.live && generation.epoch == 3);
  assert(generation.Observe(0x1000, 2, 8192));
}
