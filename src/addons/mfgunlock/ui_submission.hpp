// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "./output_state.hpp"
#include "./ui_alpha.hpp"

namespace mfgunlock::uisubmission {

enum class Path : uint32_t {
  kUnavailable = 0,
  kColorAndAlpha,
  kAlphaOnly,
};

struct Candidate {
  outputstate::UiEncoding encoding = outputstate::UiEncoding::kUnknown;
  uialpha::AlphaConvention alpha = uialpha::AlphaConvention::kUnknown;
  uint32_t format = 0;
};

inline bool SupportsAlphaExtraction(uint32_t format) {
  return format == outputstate::kFormatRgba8Unorm ||
      format == outputstate::kFormatRgba8Srgb;
}

inline Path Select(const outputstate::State& output, const Candidate& candidate,
                   bool alpha_backend_available) {
  const bool encoding_compatible =
      outputstate::IsUiEncodingCompatible(output, candidate.encoding, false);
  const bool sdr_output = output.encoding == outputstate::Encoding::kSdrSrgb ||
      output.encoding == outputstate::Encoding::kSdrLinear;

  // Preserve the pre-Phase-6 automatic SDR path. A stable, output-compatible
  // candidate was already submitted directly as Color+Alpha in SDR, and the
  // new alpha-evidence model must not turn incomplete blend observation into
  // an SDR regression. HDR remains on the stricter alpha-only path below.
  if (sdr_output && encoding_compatible && candidate.format != 0) {
    return Path::kColorAndAlpha;
  }

  if (candidate.alpha == uialpha::AlphaConvention::kPremultiplied &&
      encoding_compatible) {
    return Path::kColorAndAlpha;
  }

  // Alpha is coverage, not color. Blend-state evidence can prove accumulated
  // opacity without proving shader RGB semantics, so automatic discovery uses
  // the alpha-only path unless stronger premultiplied-color evidence exists.
  const bool alpha_proven =
      candidate.alpha == uialpha::AlphaConvention::kAlphaOnly ||
      candidate.alpha == uialpha::AlphaConvention::kPremultiplied;
  if (alpha_proven && alpha_backend_available &&
      SupportsAlphaExtraction(candidate.format)) {
    return Path::kAlphaOnly;
  }

  return Path::kUnavailable;
}

inline const char* PathName(Path path) {
  switch (path) {
    case Path::kColorAndAlpha: return "UI Color+Alpha";
    case Path::kAlphaOnly: return "UI Alpha";
    default: return "unavailable";
  }
}

}  // namespace mfgunlock::uisubmission
