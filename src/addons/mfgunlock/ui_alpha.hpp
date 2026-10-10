// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace mfgunlock::uialpha {

enum class AlphaConvention : uint32_t {
  kUnknown = 0,
  kStraight,
  kPremultiplied,
  kAlphaOnly,
  kOpaque,
};

enum class BlendFactor : uint32_t {
  kUnknown = 0,
  kZero,
  kOne,
  kSourceAlpha,
  kOneMinusSourceAlpha,
};

enum class BlendOp : uint32_t {
  kUnknown = 0,
  kAdd,
};

struct BlendState {
  bool alpha_to_coverage = false;
  bool blend_enabled = false;
  bool logic_op_enabled = false;
  BlendFactor source_color = BlendFactor::kUnknown;
  BlendFactor dest_color = BlendFactor::kUnknown;
  BlendOp color_op = BlendOp::kUnknown;
  BlendFactor source_alpha = BlendFactor::kUnknown;
  BlendFactor dest_alpha = BlendFactor::kUnknown;
  BlendOp alpha_op = BlendOp::kUnknown;
  uint8_t write_mask = 0;
};

struct Evidence {
  uint32_t draw_count = 0;
  uint32_t accumulated_alpha_draws = 0;
  uint32_t unproven_draws = 0;
};

inline bool WritesColorAndAlpha(const BlendState& state) {
  return (state.write_mask & 0x0f) == 0x0f;
}

// Starting from a transparent target, this blend equation proves that the
// destination alpha accumulates conventional UI coverage. It intentionally does
// not claim anything about the RGB transfer or shader-side premultiplication.
inline bool ProvesAccumulatedAlpha(const BlendState& state) {
  return !state.alpha_to_coverage && state.blend_enabled &&
         !state.logic_op_enabled && WritesColorAndAlpha(state) &&
         state.source_alpha == BlendFactor::kOne &&
         state.dest_alpha == BlendFactor::kOneMinusSourceAlpha &&
         state.alpha_op == BlendOp::kAdd;
}

inline void ObserveDraw(Evidence& evidence, const BlendState* state) {
  ++evidence.draw_count;
  if (state != nullptr && ProvesAccumulatedAlpha(*state)) {
    ++evidence.accumulated_alpha_draws;
  } else {
    ++evidence.unproven_draws;
  }
}

inline AlphaConvention Resolve(const Evidence& evidence) {
  if (evidence.draw_count == 0 || evidence.unproven_draws != 0 ||
      evidence.accumulated_alpha_draws != evidence.draw_count) {
    return AlphaConvention::kUnknown;
  }
  return AlphaConvention::kAlphaOnly;
}

inline const char* AlphaConventionName(AlphaConvention convention) {
  switch (convention) {
    case AlphaConvention::kStraight: return "straight";
    case AlphaConvention::kPremultiplied: return "premultiplied";
    case AlphaConvention::kAlphaOnly: return "alpha-only";
    case AlphaConvention::kOpaque: return "opaque";
    default: return "unknown";
  }
}

}  // namespace mfgunlock::uialpha
