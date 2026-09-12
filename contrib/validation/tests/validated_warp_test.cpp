// SPDX-License-Identifier: MIT
// Exercises the production Validated Warp Blend PTX rewrite without embedding
// or redistributing an NVIDIA kernel payload.

#include "../../../src/addons/mfgunlock/validated_warp.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace warp = mfgunlock::validatedwarp::internal;

namespace {

unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (!condition) throw std::runtime_error(reason);
}

std::string Fixture() {
  return ".version 8.7\n"
         ".target sm_120\n"
         ".reg .pred %p<260>;\n"
         ".visible .entry Kernel_BlendCandidatesFused() {\n"
         "ld.param.u8 %rs8, [%rd6+220];\n"
         "ret;\n"
         "}\n";
}

}  // namespace

int main() {
  std::string why;
  std::string ptx = Fixture();
  Check(warp::RewriteValidatedWarpBlend(ptx, why), "validated warp rewrite");
  Check(ptx.find("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != std::string::npos,
        "rewrite marker");
  Check(ptx.find(".reg .pred %qv<7>;") != std::string::npos,
        "predicate registers");
  Check(ptx.find(".reg .f32 %qf<12>;") != std::string::npos,
        "float registers");
  Check(ptx.find("ld.param.u8 %rs8, [%rd6+220];") != std::string::npos,
        "original insertion anchor preserved");

  why.clear();
  Check(!warp::RewriteValidatedWarpBlend(ptx, why), "double rewrite rejected");
  Check(why == "validated-warp PTX is already modified", "double rewrite reason");

  why.clear();
  std::string ambiguous = Fixture() + ".reg .pred %p<260>;\n";
  Check(!warp::RewriteValidatedWarpBlend(ambiguous, why), "ambiguous anchor rejected");
  Check(why == "required PTX anchor is ambiguous", "ambiguous anchor reason");

  why.clear();
  std::string missing = Fixture();
  const auto insertion = missing.find("ld.param.u8 %rs8, [%rd6+220];\n");
  missing.erase(insertion, std::string("ld.param.u8 %rs8, [%rd6+220];\n").size());
  Check(!warp::RewriteValidatedWarpBlend(missing, why), "missing anchor rejected");
  Check(why == "required PTX anchor is missing", "missing anchor reason");

  std::cout << "validated_warp_test: " << g_checks << " checks passed\n";
  return 0;
}
