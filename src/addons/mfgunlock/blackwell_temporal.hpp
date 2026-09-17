/*
 * Full Blackwell temporal framework retarget for Ampere.
 * SPDX-License-Identifier: MIT
 *
 * The exact DLSS-G 310.9.1 Blackwell motion-vector, inpaint and inpaint-
 * decision PTX programs are rebuilt as sm_86 fatbins. Intermediate Scatter
 * Retention is an optional rewrite of the motion-vector program inside this
 * complete temporal backend; it is never applied as a standalone redirect.
 */

#pragma once

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "./validated_warp.hpp"

namespace mfgunlock::blackwelltemporal {

using Redirect = validatedwarp::Redirect;

enum class KernelRole {
  kMotionVector,
  kInpaint,
  kInpaintDecision,
};

inline const char* RoleName(KernelRole role) {
  switch (role) {
    case KernelRole::kMotionVector: return "motion-vector";
    case KernelRole::kInpaint: return "inpaint";
    case KernelRole::kInpaintDecision: return "inpaint-decision";
    default: return "unknown";
  }
}

struct PreparedKernel {
  KernelRole role = KernelRole::kMotionVector;
  validatedwarp::internal::PreparedRedirect redirect;
};

struct Plan {
  std::array<PreparedKernel, 3> kernels{};
  bool intermediate_scatter = false;
  bool ready = false;
};

struct Result {
  bool detected = false;
  bool applied = false;
  bool intermediate_scatter = false;
  bool motion_vector = false;
  bool inpaint = false;
  bool inpaint_decision = false;
  bool fallback_safe = true;
  std::string detail;
};

namespace internal {

constexpr validatedwarp::internal::PtxProfile kMotionVectorProfile = {
    validatedwarp::internal::kBlackwellArch,
    0u,
    90731u,
    0xb1a2811b29625d41ull,
    8u,
    "Kernel_EstimateIntermMvecsScatter",
    "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];"};

constexpr validatedwarp::internal::PtxProfile kInpaintProfile = {
    validatedwarp::internal::kBlackwellArch,
    0u,
    26439u,
    0x546151924160b69bull,
    8u,
    "Kernel_Prev2CurrUnpackPull",
    ".entry Kernel_Prev2CurrUnpackPull("};

constexpr validatedwarp::internal::PtxProfile kInpaintDecisionProfile = {
    validatedwarp::internal::kBlackwellArch,
    0u,
    23116u,
    0x9b47635b91b2436bull,
    8u,
    "Kernel_OutputPull",
    ".entry Kernel_OutputPull("};

struct KernelSpec {
  KernelRole role;
  const validatedwarp::internal::PtxProfile* profile;
  size_t ada_cubin_slot_size;
  uint64_t ada_cubin_fnv1a64;
};

constexpr std::array<KernelSpec, 3> kKernelSpecs = {{
    {KernelRole::kMotionVector, &kMotionVectorProfile, 39968u,
     0x9642092def23b3dfull},
    {KernelRole::kInpaint, &kInpaintProfile, 17568u,
     0x1ba6454ab039f9ddull},
    {KernelRole::kInpaintDecision, &kInpaintDecisionProfile, 15136u,
     0xc4a5eb4a8694f835ull},
}};

inline bool RewriteIntermediateScatter(std::string& ptx, std::string& why) {
  if (ptx.find("MFGUNLOCK_INTERMEDIATE_SCATTER_V1") != std::string::npos) {
    why = "intermediate-scatter PTX is already modified";
    return false;
  }
  constexpr char kAnchor[] =
      "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n";
  return validatedwarp::internal::ReplaceOnce(
      ptx, kAnchor,
      std::string(kAnchor) +
          "mul.ftz.f32 %f2, %f2, 0f3F000000; // MFGUNLOCK_INTERMEDIATE_SCATTER_V1\n",
      why);
}

inline bool ValidateAdaCubinSlot(
    const validatedwarp::internal::PreparedRedirect& prepared,
    const KernelSpec& spec, std::string& why) {
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse(std::span<const unsigned char>(
          prepared.located.address, prepared.located.size), entries, end) ||
      end != prepared.located.size || entries.size() != 3) {
    why = "temporal fatbin layout changed";
    return false;
  }
  const auto& cubin = entries[2];
  if (cubin.kind != 2 || cubin.architecture != validatedwarp::internal::kAdaArch ||
      cubin.payload_bytes != spec.ada_cubin_slot_size) {
    why = "original Ada cubin slot layout changed";
    return false;
  }
  const auto* payload = prepared.located.address + cubin.PayloadOffset();
  if (validatedwarp::internal::Fnv1a64(payload, cubin.payload_bytes) !=
      spec.ada_cubin_fnv1a64) {
    why = "original Ada cubin slot fingerprint changed";
    return false;
  }
  return true;
}

inline bool ValidateDisjointPlan(const Plan& plan, std::string& why) {
  for (size_t left = 0; left < plan.kernels.size(); ++left) {
    const auto& lhs = plan.kernels[left].redirect;
    if (lhs.located.address == nullptr || lhs.slots.empty()) {
      why = "prepared temporal kernel is incomplete";
      return false;
    }
    for (size_t right = left + 1; right < plan.kernels.size(); ++right) {
      const auto& rhs = plan.kernels[right].redirect;
      if (lhs.located.address == rhs.located.address) {
        why = "two temporal roles resolved to the same provider fatbin";
        return false;
      }
      for (uint64_t* lhs_slot : lhs.slots) {
        if (std::find(rhs.slots.begin(), rhs.slots.end(), lhs_slot) != rhs.slots.end()) {
          why = "two temporal roles share a provider descriptor slot";
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace internal

inline bool Prepare(HMODULE module, bool enable_intermediate_scatter,
                    Plan& plan, Result& result, std::string& provider_version) {
  plan = {};
  result = {};
  if (!validatedwarp::IsSupportedProvider(module, provider_version, result.detail)) return false;

  for (size_t index = 0; index < internal::kKernelSpecs.size(); ++index) {
    const auto& spec = internal::kKernelSpecs[index];
    plan.kernels[index].role = spec.role;
    const auto rewrite = spec.role == KernelRole::kMotionVector && enable_intermediate_scatter
        ? internal::RewriteIntermediateScatter
        : nullptr;
    std::string detail;
    if (!validatedwarp::internal::PrepareRetargetedRedirect(
            module, *spec.profile, rewrite, plan.kernels[index].redirect, detail) ||
        !internal::ValidateAdaCubinSlot(
            plan.kernels[index].redirect, spec, detail)) {
      std::ostringstream stream;
      stream << RoleName(spec.role) << " prepare failed: " << detail;
      result.detail = stream.str();
      return false;
    }
  }

  if (!internal::ValidateDisjointPlan(plan, result.detail)) return false;
  plan.intermediate_scatter = enable_intermediate_scatter;
  plan.ready = true;
  result.detected = true;
  return true;
}

inline bool Commit(Plan& plan, std::vector<Redirect>& redirects, Result& result) {
  if (!plan.ready) {
    result.detail = "Blackwell temporal redirect plan is not ready";
    return false;
  }

  std::vector<Redirect> committed;
  std::ostringstream details;
  for (size_t index = 0; index < plan.kernels.size(); ++index) {
    auto& kernel = plan.kernels[index];
    Redirect redirect;
    std::string detail;
    const std::string label = std::string("path=Blackwell temporal sm_86; role=") +
        RoleName(kernel.role);
    if (!validatedwarp::internal::CommitPreparedRedirect(
            kernel.redirect, label, redirect, detail)) {
      const bool incomplete_rollback =
          detail.find("incomplete rollback") != std::string::npos;
      validatedwarp::Restore(committed);
      result.fallback_safe = !incomplete_rollback;
      std::ostringstream stream;
      stream << RoleName(kernel.role) << " commit failed: " << detail;
      if (!incomplete_rollback)
        stream << "; previously committed temporal redirects were rolled back";
      result.detail = stream.str();
      plan.ready = false;
      return false;
    }
    if (index != 0) details << " | ";
    details << detail;
    committed.push_back(std::move(redirect));
  }

  for (auto& redirect : committed) redirects.push_back(std::move(redirect));
  plan.ready = false;
  result.applied = true;
  result.intermediate_scatter = plan.intermediate_scatter;
  result.motion_vector = true;
  result.inpaint = true;
  result.inpaint_decision = true;
  result.detail = details.str();
  return true;
}

inline void Restore(std::vector<Redirect>& redirects) {
  validatedwarp::Restore(redirects);
}

}  // namespace mfgunlock::blackwelltemporal
