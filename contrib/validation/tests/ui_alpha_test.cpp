// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>

#include "../../../src/addons/mfgunlock/ui_alpha.hpp"

namespace {
unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (condition) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}

mfgunlock::uialpha::BlendState CanonicalStraightAlphaBlend() {
  using namespace mfgunlock::uialpha;
  BlendState state{};
  state.blend_enabled = true;
  state.source_color = BlendFactor::kSourceAlpha;
  state.dest_color = BlendFactor::kOneMinusSourceAlpha;
  state.color_op = BlendOp::kAdd;
  state.source_alpha = BlendFactor::kOne;
  state.dest_alpha = BlendFactor::kOneMinusSourceAlpha;
  state.alpha_op = BlendOp::kAdd;
  state.write_mask = 0x0f;
  return state;
}
}  // namespace

int main() {
  namespace alpha = mfgunlock::uialpha;

  auto blend = CanonicalStraightAlphaBlend();
  Check(alpha::ProvesAccumulatedAlpha(blend),
        "canonical alpha equation proves accumulated target opacity");

  auto premultiplied_source = blend;
  premultiplied_source.source_color = alpha::BlendFactor::kOne;
  Check(alpha::ProvesAccumulatedAlpha(premultiplied_source),
        "RGB blend factors do not change the independently proven alpha equation");

  auto no_alpha_write = blend;
  no_alpha_write.write_mask = 0x07;
  Check(!alpha::ProvesAccumulatedAlpha(no_alpha_write),
        "RGB-only write cannot prove target alpha semantics");

  auto logic_op = blend;
  logic_op.logic_op_enabled = true;
  Check(!alpha::ProvesAccumulatedAlpha(logic_op),
        "logic-op path fails closed");

  auto alpha_to_coverage = blend;
  alpha_to_coverage.alpha_to_coverage = true;
  Check(!alpha::ProvesAccumulatedAlpha(alpha_to_coverage),
        "alpha-to-coverage path fails closed");

  alpha::Evidence evidence{};
  alpha::ObserveDraw(evidence, &blend);
  alpha::ObserveDraw(evidence, &blend);
  Check(alpha::Resolve(evidence) == alpha::AlphaConvention::kAlphaOnly,
        "all proven draws resolve to alpha-only evidence");

  auto unproven = blend;
  unproven.source_alpha = alpha::BlendFactor::kSourceAlpha;
  alpha::ObserveDraw(evidence, &unproven);
  Check(alpha::Resolve(evidence) == alpha::AlphaConvention::kUnknown,
        "one unproven alpha draw invalidates coverage evidence");

  alpha::Evidence missing_pipeline{};
  alpha::ObserveDraw(missing_pipeline, nullptr);
  Check(alpha::Resolve(missing_pipeline) == alpha::AlphaConvention::kUnknown,
        "missing pipeline evidence remains unknown");

  alpha::Evidence empty{};
  Check(alpha::Resolve(empty) == alpha::AlphaConvention::kUnknown,
        "no observed draws cannot establish alpha convention");

  Check(std::string(alpha::AlphaConventionName(alpha::AlphaConvention::kAlphaOnly)) ==
            "alpha-only" &&
        std::string(alpha::AlphaConventionName(alpha::AlphaConvention::kUnknown)) == "unknown",
        "diagnostic names remain stable");

  std::cout << "ui_alpha_test: " << g_checks << " checks PASS\n";
  return 0;
}
