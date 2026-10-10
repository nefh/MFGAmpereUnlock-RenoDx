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
  // Versions are not inputs. The same complete surface selects the same adapter.
  assert(AssessInterposer(legacy, true, true).typed_abi == TypedInterposerAbi::kLegacy1);
  assert(!AssessInterposer(modern, false, true).admission.Available());
  assert(!AssessInterposer(modern, true, false).admission.Available());
  auto forwarded = modern;
  forwarded[0].own_code = false;
  assert(!AssessInterposer(forwarded, true, true).admission.Available());
  auto mixed = std::array<ExportSymbol, modern.size() + 1>{};
  for (size_t i = 0; i < modern.size(); ++i) mixed[i] = modern[i];
  mixed.back() = {"slSetFeatureConstants", true};
  assert(!AssessInterposer(mixed, true, true).admission.Available());

  PluginGatewayEvidence plugin{};
  plugin.named_dlssg = plugin.own_code_gateway = plugin.current_image = true;
  plugin.parent_abi_qualified = true;
  plugin.set_options = plugin.get_state = true;
  assert(PluginFunctions(plugin, TypedInterposerAbi::kModern2).Available());
  plugin.get_state = false;
  assert(!PluginFunctions(plugin, TypedInterposerAbi::kModern2).Available());
  plugin.get_state = true;
  plugin.own_code_gateway = false;
  assert(!GatewayLookup(plugin).Available());  // structural match is not enough
  plugin.own_code_gateway = true;
  plugin.on_load = plugin.on_startup = true;
  assert(PluginFunctions(plugin, TypedInterposerAbi::kLegacy1).state == FeatureState::kDegraded);

  DynamicEvidence dynamic{};
  dynamic.architecture_supports = dynamic.target_ready = dynamic.provider_qualified = true;
  dynamic.typed_modern_interposer = dynamic.typed_modern_plugin = true;
  dynamic.options_v5 = dynamic.state_v4 = dynamic.hook_entries_current = true;
  dynamic.ceiling_observed = dynamic.d3d12 = true;
  assert(DynamicProbe(dynamic).Available());
  assert(DynamicStack(dynamic).state == FeatureState::kUnknown);
  dynamic.api_support_observed = true;
  assert(DynamicStack(dynamic).state == FeatureState::kUnavailable);
  dynamic.api_supports_dynamic = true;
  assert(DynamicStack(dynamic).Available());
  dynamic.provider_qualified = false;
  assert(!DynamicStack(dynamic).Available());
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
