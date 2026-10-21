/*
 * Full Blackwell temporal framework for Ada, Ampere and Turing.
 * SPDX-License-Identifier: MIT
 *
 * Structurally resolved motion-vector, inpaint and inpaint-decision PTX programs are rebuilt as sm_89, sm_86 or sm_75 fatbins. Intermediate Scatter
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

enum class BoundaryArtifactMode : unsigned int {
  kOff = 0,
  kBalanced = 1,
  kAggressive = 2,
};

inline BoundaryArtifactMode ParseBoundaryArtifactMode(int value) {
  switch (value) {
    case static_cast<int>(BoundaryArtifactMode::kBalanced):
      return BoundaryArtifactMode::kBalanced;
    case static_cast<int>(BoundaryArtifactMode::kAggressive):
      return BoundaryArtifactMode::kAggressive;
    default:
      return BoundaryArtifactMode::kOff;
  }
}

inline const char* BoundaryArtifactModeName(BoundaryArtifactMode mode) {
  switch (mode) {
    case BoundaryArtifactMode::kBalanced: return "Balanced";
    case BoundaryArtifactMode::kAggressive: return "Aggressive";
    default: return "Off";
  }
}

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
  BoundaryArtifactMode boundary_mode = BoundaryArtifactMode::kOff;
  bool ready = false;
  uint32_t target_sm = 86;
};

struct Result {
  bool detected = false;
  bool applied = false;
  bool intermediate_scatter = false;
  BoundaryArtifactMode boundary_mode = BoundaryArtifactMode::kOff;
  bool motion_vector = false;
  bool inpaint = false;
  bool inpaint_decision = false;
  bool fallback_safe = true;
  std::string detail;
};

namespace internal {

inline constexpr auto& kMotionVectorProfile = profiles::kMotionVectorPtx;

inline constexpr auto& kInpaintProfile = profiles::kInpaintPtx;

inline constexpr auto& kInpaintDecisionProfile = profiles::kInpaintDecisionPtx;

struct KernelSpec {
  KernelRole role;
  const validatedwarp::internal::PtxProfile* profile;
  size_t ada_cubin_slot_size;
  uint64_t ada_cubin_fnv1a64;
};

constexpr std::array<KernelSpec, 3> kKernelSpecs = {{
    {KernelRole::kMotionVector, &kMotionVectorProfile, profiles::kQualityKernels[0].ada_cubin_bytes,
     profiles::kQualityKernels[0].ada_cubin_hash},
    {KernelRole::kInpaint, &kInpaintProfile, profiles::kQualityKernels[1].ada_cubin_bytes,
     profiles::kQualityKernels[1].ada_cubin_hash},
    {KernelRole::kInpaintDecision, &kInpaintDecisionProfile, profiles::kQualityKernels[2].ada_cubin_bytes,
     profiles::kQualityKernels[2].ada_cubin_hash},
}};

constexpr char kMotionDivisorLoad[] =
    "ld.param.f32 %f2, [Kernel_EstimateIntermMvecsScatter_param_0+120];\n";
constexpr char kMotionRegisterAnchor[] = ".reg .pred %p<656>;\n";
constexpr char kBoundaryBalancedMarker[] = "MFGUNLOCK_BOUNDARY_ARTIFACT_BALANCED_V1";
constexpr char kBoundaryAggressiveMarker[] = "MFGUNLOCK_BOUNDARY_ARTIFACT_AGGRESSIVE_V1";

struct BoundaryDirection {
  const char* marker;
  const char* anchor;
  const char* center_x;
  const char* center_y;
  const char* center_depth;
  std::array<std::array<const char*, 3>, 4> neighbors;
  const char* motion_length;
  bool reload_divisor;
};

// Shared-tile recipe. Unique local instructions and declared native operand
// definitions guard this mapping; whole-program identities never admit it.
constexpr std::array<BoundaryDirection, 2> kBoundaryDirections = {{
    {"CURR_TO_PREV", "fma.rn.ftz.f32 %f14, %f7, %f7, %f157;\n",
     "%f7", "%f8", "%f9",
     {{{"%f55", "%f56", "%f57"}, {"%f78", "%f79", "%f80"},
       {"%f97", "%f98", "%f99"}, {"%f120", "%f121", "%f122"}}},
     "%f14", false},
    {"PREV_TO_CURR", "fma.rn.ftz.f32 %f22, %f15, %f15, %f942;\n",
     "%f15", "%f16", "%f17",
     {{{"%f840", "%f841", "%f842"}, {"%f863", "%f864", "%f865"},
       {"%f882", "%f883", "%f884"}, {"%f905", "%f906", "%f907"}}},
     "%f22", true},
}};

inline bool MotionVectorAlreadyModified(const std::string& ptx) {
  return ptx.find("MFGUNLOCK_INTERMEDIATE_SCATTER_V1") != std::string::npos ||
      ptx.find(kBoundaryBalancedMarker) != std::string::npos ||
      ptx.find(kBoundaryAggressiveMarker) != std::string::npos;
}

inline void AppendBoundaryNeighbor(std::ostringstream& stream,
                                   const BoundaryDirection& direction,
                                   const std::array<const char*, 3>& neighbor,
                                   const char* depth_limit, bool aggressive) {
  stream << "sub.ftz.f32 %qgf3, " << neighbor[0] << ", " << direction.center_x << ";\n"
         << "sub.ftz.f32 %qgf4, " << neighbor[1] << ", " << direction.center_y << ";\n"
         << "mul.ftz.f32 %qgf5, %qgf4, %qgf4;\n"
         << "fma.rn.ftz.f32 %qgf5, %qgf3, %qgf3, %qgf5;\n"
         << "div.approx.ftz.f32 %qgf6, %qgf5, %qgf2;\n"
         << "sub.ftz.f32 %qgf6, 0f3F800000, %qgf6;\n"
         << "setp.gt.f32 %qgp0, %qgf6, 0f00000000;\n"
         << "sub.ftz.f32 %qgf7, " << neighbor[2] << ", " << direction.center_depth << ";\n"
         << "abs.ftz.f32 %qgf7, %qgf7;\n"
         << "setp.lt.and.f32 %qgp0, %qgf7, " << depth_limit << ", %qgp0;\n"
         << "@!%qgp0 mov.f32 %qgf6, 0f00000000;\n";
  if (aggressive) {
    stream << "add.f32 %qgf0, %qgf0, %qgf6;\n";
  } else {
    stream << "min.ftz.f32 %qgf6, %qgf6, 0f3F800000;\n"
           << "max.f32 %qgf0, %qgf0, %qgf6;\n";
  }
}

inline void AppendBoundaryScale(std::ostringstream& stream, bool aggressive) {
  if (aggressive) {
    stream << "sub.f32 %qgf0, %qgf0, 0f3F800000;\n"
           << "max.f32 %qgf0, %qgf0, 0f00000000;\n"
           << "min.f32 %qgf0, %qgf0, 0f3F800000;\n"
           << "mul.f32 %qgf0, %qgf0, %qgf0;\n"
           << "fma.rn.f32 %qgf11, %qgf0, 0fBE800000, 0f3F800000;\n";
  } else {
    stream << "fma.rn.f32 %qgf11, %qgf0, 0fBF000000, 0f3F800000;\n";
  }
  stream << "mul.ftz.f32 %f2, %f2, %qgf11;\n";
}

inline std::string BuildBoundaryProgram(const BoundaryDirection& direction,
                                        BoundaryArtifactMode mode) {
  const bool aggressive = mode == BoundaryArtifactMode::kAggressive;
  const char* depth_limit = aggressive ? "0f40000000" : "0f40400000";
  std::ostringstream stream;
  stream << "// " << (aggressive ? kBoundaryAggressiveMarker : kBoundaryBalancedMarker)
         << '_' << direction.marker << '\n';
  if (direction.reload_divisor) stream << kMotionDivisorLoad;
  stream << "mov.f32 %qgf0, 0f00000000;\n"
         << "div.approx.ftz.f32 %qgf2, " << direction.motion_length << ", %f2;\n"
         << "max.ftz.f32 %qgf2, %qgf2, 0f3F800000;\n";
  for (const auto& neighbor : direction.neighbors)
    AppendBoundaryNeighbor(stream, direction, neighbor, depth_limit, aggressive);
  AppendBoundaryScale(stream, aggressive);
  return stream.str();
}

inline bool FindDivisorLoad(const std::string& source, size_t& start, size_t& bytes,
                            std::string& reg, std::string& why) {
  std::string code;
  if (!validatedwarp::internal::MaskPtx(source, code)) {
    why = "motion-vector lexical structure is invalid"; return false;
  }
  size_t matches = 0;
  for (size_t at = 0; at < code.size();) {
    const auto nl = code.find('\n', at);
    const auto end = nl == code.npos ? code.size() : nl;
    auto line = ptx::Trim(std::string_view(code).substr(at, end - at));
    if (line.starts_with("ld.param.f32") && line.size() > 12 &&
        (line[12] == ' ' || line[12] == '\t')) {
      auto rest = ptx::Trim(line.substr(12));
      const auto comma = rest.find(',');
      if (comma != rest.npos && validatedwarp::internal::CompactCode(rest.substr(comma + 1)) ==
          "[Kernel_EstimateIntermMvecsScatter_param_0+120];") {
        const auto destination = ptx::Trim(rest.substr(0, comma));
        if (destination.starts_with("%f")) {
          start = at; bytes = end - at + (nl != code.npos);
          reg = destination; ++matches;
        }
      }
    }
    at = end + (nl != code.npos);
  }
  if (matches != 1) { why = matches ? "motion-vector divisor load is ambiguous" :
      "motion-vector divisor load is missing"; return false; }
  return true;
}

inline bool RewriteMotionVector(std::string& source, BoundaryArtifactMode mode,
                                std::string& why) {
  if (MotionVectorAlreadyModified(source)) {
    why = "motion-vector PTX is already modified"; return false;
  }
  size_t divisor_at = 0, divisor_bytes = 0;
  std::string divisor;
  if (!FindDivisorLoad(source, divisor_at, divisor_bytes, divisor, why)) return false;
  const std::string_view divisor_operand = divisor;
  if (!validatedwarp::internal::LocalOperandsDefined(source, divisor_at + divisor_bytes,
      {&divisor_operand, 1}, why)) return false;
  if (mode == BoundaryArtifactMode::kOff) {
    source.insert(divisor_at + divisor_bytes, "mul.ftz.f32 " + divisor + ", " + divisor +
        ", 0f3F000000; // MFGUNLOCK_INTERMEDIATE_SCATTER_V1\n");
    return true;
  }
  if (mode != BoundaryArtifactMode::kBalanced && mode != BoundaryArtifactMode::kAggressive) {
    why = "unknown boundary artifact mode"; return false;
  }
  std::string code;
  if (!validatedwarp::internal::MaskPtx(source, code)) {
    why = "motion-vector lexical structure is invalid"; return false;
  }
  std::vector<std::pair<size_t, std::string>> injections;
  for (const auto& direction : kBoundaryDirections) {
    const auto at = code.find(direction.anchor);
    if (at == code.npos || code.find(direction.anchor, at + 1) != code.npos) {
      why = at == code.npos ? "required PTX anchor is missing" : "required PTX anchor is ambiguous";
      return false;
    }
    std::vector<std::string_view> operands = {direction.center_x, direction.center_y,
        direction.center_depth, direction.motion_length, divisor};
    for (const auto& neighbor : direction.neighbors)
      for (const auto* operand : neighbor) operands.push_back(operand);
    if (!validatedwarp::internal::LocalOperandsDefined(source, at + std::strlen(direction.anchor),
        operands, why)) return false;
  }
  unsigned predicates = 0; size_t declaration_end = 0;
  if (!validatedwarp::internal::QualityRegisterLimit(code, ".pred", "%p", predicates, declaration_end)) {
    why = "motion-vector predicate declaration is ambiguous"; return false;
  }
  const auto qgp = validatedwarp::internal::FreshRegisterPrefix(source, "%qgp");
  const auto qgf = validatedwarp::internal::FreshRegisterPrefix(source, "%qgf");
  std::string result = source;
  for (const auto& direction : kBoundaryDirections) {
    std::string program = BuildBoundaryProgram(direction, mode);
    // BuildBoundaryProgram's divisor is a template operand, resolved from the
    // actual parameter load. Token replacement avoids aliasing %f20 etc.
    for (size_t at = 0; (at = program.find("%f2", at)) != program.npos;) {
      const auto end = at + 3;
      if (end == program.size() || program[end] < '0' || program[end] > '9') {
        program.replace(at, 3, divisor); at += divisor.size();
      } else at = end;
    }
    validatedwarp::internal::RenameGeneratedPrefix(program, "%qgp", qgp);
    validatedwarp::internal::RenameGeneratedPrefix(program, "%qgf", qgf);
    injections.emplace_back(code.find(direction.anchor) + std::strlen(direction.anchor), std::move(program));
  }
  std::sort(injections.begin(), injections.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& insertion : injections) result.insert(insertion.first, insertion.second);
  result.insert(declaration_end, "\n.reg .pred " + qgp + "<2>;\n.reg .f32 " + qgf + "<12>;\n");
  source.swap(result); return true;
}

inline bool RewriteMotionVectorOff(std::string& ptx, std::string& why) {
  return RewriteMotionVector(ptx, BoundaryArtifactMode::kOff, why);
}

inline bool RewriteMotionVectorBalanced(std::string& ptx, std::string& why) {
  return RewriteMotionVector(ptx, BoundaryArtifactMode::kBalanced, why);
}

inline bool RewriteMotionVectorAggressive(std::string& ptx, std::string& why) {
  return RewriteMotionVector(ptx, BoundaryArtifactMode::kAggressive, why);
}

inline validatedwarp::internal::RewritePtxCallback MotionVectorRewrite(
    BoundaryArtifactMode mode) {
  switch (mode) {
    case BoundaryArtifactMode::kOff: return RewriteMotionVectorOff;
    case BoundaryArtifactMode::kBalanced: return RewriteMotionVectorBalanced;
    case BoundaryArtifactMode::kAggressive: return RewriteMotionVectorAggressive;
    default: return nullptr;
  }
}

// Other native image payloads are copied intact; their fingerprints or sizes
// cannot qualify/refuse a PTX rewrite. Verify the actual rebuilt selection.
inline bool ValidateAdaCubinSlot(
    const validatedwarp::internal::PreparedRedirect& prepared,
    const KernelSpec&, std::string& why) {
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (prepared.rebuilt.empty() || !fatbin::Parse(prepared.rebuilt, entries, end) ||
      end != prepared.rebuilt.size()) {
    why = "temporal replacement has invalid container bounds";
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
                    BoundaryArtifactMode boundary_mode,
                    Plan& plan, Result& result, std::string& provider_version,
                    uint32_t target_sm = 86) {
  plan = {};
  result = {};
  if (target_sm == 0) {
    result.detail = "temporal target is unknown";
    return false;
  }
  plan.target_sm = target_sm;
  if (!validatedwarp::IsSupportedProvider(module, provider_version, result.detail)) return false;

  const auto motion_rewrite = enable_intermediate_scatter
      ? internal::MotionVectorRewrite(boundary_mode)
      : nullptr;
  if (enable_intermediate_scatter && motion_rewrite == nullptr) {
    result.detail = "unknown boundary artifact mode";
    return false;
  }

  for (size_t index = 0; index < internal::kKernelSpecs.size(); ++index) {
    const auto& spec = internal::kKernelSpecs[index];
    plan.kernels[index].role = spec.role;
    const auto rewrite = spec.role == KernelRole::kMotionVector ? motion_rewrite : nullptr;
    std::string detail;
    if (!validatedwarp::internal::PrepareRetargetedRedirect(
            module, *spec.profile, rewrite, plan.kernels[index].redirect, detail, target_sm) ||
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
  plan.boundary_mode = enable_intermediate_scatter
      ? boundary_mode
      : BoundaryArtifactMode::kOff;
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
  committed.reserve(plan.kernels.size());
  redirects.reserve(redirects.size() + plan.kernels.size());
  std::ostringstream details;
  for (size_t index = 0; index < plan.kernels.size(); ++index) {
    auto& kernel = plan.kernels[index];
    Redirect redirect;
    std::string detail;
    const std::string label = "path=Blackwell temporal sm_" +
        std::to_string(plan.target_sm) + "; role=" + RoleName(kernel.role);
    if (!validatedwarp::internal::CommitPreparedRedirect(
            kernel.redirect, label, redirect, detail)) {
      if (redirect.allocation != nullptr) committed.push_back(std::move(redirect));
      result.fallback_safe = validatedwarp::Restore(committed);
      for (auto& pending : committed) redirects.push_back(std::move(pending));
      std::ostringstream stream;
      stream << RoleName(kernel.role) << " commit failed: " << detail;
      if (result.fallback_safe)
        stream << "; previously committed temporal redirects were rolled back";
      else
        stream << "; incomplete rollback; temporal fallback blocked";
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
  result.boundary_mode = plan.boundary_mode;
  result.motion_vector = true;
  result.inpaint = true;
  result.inpaint_decision = true;
  result.detail = details.str();
  return true;
}

inline bool Restore(std::vector<Redirect>& redirects) {
  return validatedwarp::Restore(redirects);
}

}  // namespace mfgunlock::blackwelltemporal
