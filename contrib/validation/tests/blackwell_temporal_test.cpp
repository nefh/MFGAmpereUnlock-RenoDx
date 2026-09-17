// SPDX-License-Identifier: MIT
// Exercises the exact full-temporal profiles and Intermediate Scatter rewrite
// without embedding or redistributing NVIDIA kernel payloads.

#include "../../../src/addons/mfgunlock/blackwell_temporal.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace temporal = mfgunlock::blackwelltemporal;
namespace internal = mfgunlock::blackwelltemporal::internal;

namespace {

unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (!condition) throw std::runtime_error(reason);
}

std::string MotionFixture() {
  return ".version 8.7\n"
         ".target sm_120\n"
         ".visible .entry Kernel_EstimateIntermMvecsScatter() {\n"
         "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n"
         "ret;\n"
         "}\n";
}

}  // namespace

int main() {
  Check(internal::kKernelSpecs.size() == 3, "three complete temporal roles");
  Check(internal::kMotionVectorProfile.normalized_size == 90731u,
        "motion-vector PTX size");
  Check(internal::kMotionVectorProfile.raw_fnv1a64 == 0xb1a2811b29625d41ull,
        "motion-vector PTX hash");
  Check(internal::kInpaintProfile.normalized_size == 26439u,
        "inpaint PTX size");
  Check(internal::kInpaintProfile.raw_fnv1a64 == 0x546151924160b69bull,
        "inpaint PTX hash");
  Check(internal::kInpaintDecisionProfile.normalized_size == 23116u,
        "inpaint-decision PTX size");
  Check(internal::kInpaintDecisionProfile.raw_fnv1a64 == 0x9b47635b91b2436bull,
        "inpaint-decision PTX hash");
  Check(internal::kMotionVectorProfile.descriptor_references == 8u,
        "motion-vector descriptor count");
  Check(internal::kInpaintProfile.descriptor_references == 8u,
        "inpaint descriptor count");
  Check(internal::kInpaintDecisionProfile.descriptor_references == 8u,
        "inpaint-decision descriptor count");

  Check(internal::kKernelSpecs[0].ada_cubin_slot_size == 39968u &&
            internal::kKernelSpecs[0].ada_cubin_fnv1a64 == 0x9642092def23b3dfull,
        "motion-vector Ada cubin identity");
  Check(internal::kKernelSpecs[1].ada_cubin_slot_size == 17568u &&
            internal::kKernelSpecs[1].ada_cubin_fnv1a64 == 0x1ba6454ab039f9ddull,
        "inpaint Ada cubin identity");
  Check(internal::kKernelSpecs[2].ada_cubin_slot_size == 15136u &&
            internal::kKernelSpecs[2].ada_cubin_fnv1a64 == 0xc4a5eb4a8694f835ull,
        "inpaint-decision Ada cubin identity");

  std::string why;
  std::string ptx = MotionFixture();
  Check(internal::RewriteIntermediateScatter(ptx, why),
        "intermediate scatter rewrite");
  Check(ptx.find("MFGUNLOCK_INTERMEDIATE_SCATTER_V1") != std::string::npos,
        "rewrite marker");
  Check(ptx.find("0f3F000000") != std::string::npos,
        "retention factor");
  Check(ptx.find("ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];") !=
            std::string::npos,
        "source load preserved");

  why.clear();
  Check(!internal::RewriteIntermediateScatter(ptx, why), "double rewrite rejected");
  Check(why == "intermediate-scatter PTX is already modified",
        "double rewrite reason");

  why.clear();
  std::string ambiguous = MotionFixture() +
      "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n";
  Check(!internal::RewriteIntermediateScatter(ambiguous, why),
        "ambiguous anchor rejected");
  Check(why == "required PTX anchor is ambiguous", "ambiguous anchor reason");

  why.clear();
  std::string missing = MotionFixture();
  const std::string anchor =
      "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n";
  const auto offset = missing.find(anchor);
  missing.erase(offset, anchor.size());
  Check(!internal::RewriteIntermediateScatter(missing, why),
        "missing anchor rejected");
  Check(why == "required PTX anchor is missing", "missing anchor reason");

  Check(std::string(temporal::RoleName(temporal::KernelRole::kMotionVector)) ==
            "motion-vector",
        "motion-vector role name");
  Check(std::string(temporal::RoleName(temporal::KernelRole::kInpaint)) == "inpaint",
        "inpaint role name");
  Check(std::string(temporal::RoleName(temporal::KernelRole::kInpaintDecision)) ==
            "inpaint-decision",
        "inpaint-decision role name");

  std::cout << "blackwell_temporal_test: " << g_checks << " checks passed\n";
  return 0;
}
