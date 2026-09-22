// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace mfgunlock::outputstate {

inline constexpr uint32_t kUnknown32 = UINT32_MAX;

// ReShade color-space enum values are stable across the supported API history,
// even though the symbolic names changed in newer headers.
inline constexpr uint32_t kColorSpaceSrgbNonlinear = 1;
inline constexpr uint32_t kColorSpaceScRgbLinear = 2;
inline constexpr uint32_t kColorSpaceHdr10Pq = 3;
inline constexpr uint32_t kColorSpaceHdr10Hlg = 4;

enum class Encoding : uint32_t {
  kUnknown = 0,
  kSdrSrgb,
  kSdrLinear,
  kHdr10Pq,
  kScRgbLinear,
  kHdrOther,
};

enum class FormatKind : uint32_t {
  kUnknown = 0,
  kRgba8Unorm,
  kRgba8Srgb,
  kRgb10A2Unorm,
  kRgba16Float,
  kOther,
};

enum class UiEncoding : uint32_t {
  kUnknown = 0,
  kAlphaOnly,
  kSdrSrgb,
  kSdrLinear,
  kOutputEncoded,
};

// These values match the DXGI/native-format values already consumed by the
// Streamline integration and have been stable in ReShade's format enum.
inline constexpr uint32_t kFormatRgba16Float = 10;
inline constexpr uint32_t kFormatRgb10A2Unorm = 24;
inline constexpr uint32_t kFormatRgba8Unorm = 28;
inline constexpr uint32_t kFormatRgba8Srgb = 29;

inline FormatKind ClassifyFormat(uint32_t format, bool known = true) {
  if (!known || format == 0 || format == kUnknown32) return FormatKind::kUnknown;
  switch (format) {
    case kFormatRgba16Float: return FormatKind::kRgba16Float;
    case kFormatRgb10A2Unorm: return FormatKind::kRgb10A2Unorm;
    case kFormatRgba8Unorm: return FormatKind::kRgba8Unorm;
    case kFormatRgba8Srgb: return FormatKind::kRgba8Srgb;
    default: return FormatKind::kOther;
  }
}

struct State {
  Encoding encoding = Encoding::kUnknown;
  FormatKind format_kind = FormatKind::kUnknown;
  uint32_t format = kUnknown32;
  uint32_t color_space = kUnknown32;
  bool format_known = false;
  bool color_space_known = false;
  bool hdr = false;
  bool wide_gamut = false;
  bool linear = false;
  bool known = false;
  bool dlssg_supported = false;
  uint64_t revision = 0;
};

inline bool Equivalent(const State& lhs, const State& rhs) {
  return lhs.encoding == rhs.encoding && lhs.format_kind == rhs.format_kind &&
         lhs.format == rhs.format && lhs.color_space == rhs.color_space &&
         lhs.format_known == rhs.format_known &&
         lhs.color_space_known == rhs.color_space_known && lhs.hdr == rhs.hdr &&
         lhs.wide_gamut == rhs.wide_gamut && lhs.linear == rhs.linear &&
         lhs.known == rhs.known && lhs.dlssg_supported == rhs.dlssg_supported;
}

inline State Classify(FormatKind format_kind, uint32_t format, bool format_known,
                      uint32_t color_space, bool color_space_known) {
  State state{};
  state.format_kind = format_kind;
  state.format = format_known ? format : kUnknown32;
  state.color_space = color_space_known ? color_space : kUnknown32;
  state.format_known = format_known;
  state.color_space_known = color_space_known;
  if (!color_space_known) return state;

  switch (color_space) {
    case kColorSpaceSrgbNonlinear:
      state.encoding = Encoding::kSdrSrgb;
      state.known = true;
      state.dlssg_supported = true;
      break;
    case kColorSpaceScRgbLinear:
      state.encoding = Encoding::kScRgbLinear;
      state.hdr = true;
      state.wide_gamut = true;
      state.linear = true;
      state.known = true;
      // NVIDIA documents FP16/scRGB as unsupported by DLSS-G.
      state.dlssg_supported = false;
      break;
    case kColorSpaceHdr10Pq:
      state.encoding = Encoding::kHdr10Pq;
      state.hdr = true;
      state.wide_gamut = true;
      state.known = true;
      // The documented DLSS-G HDR path is UINT10/RGB10 + HDR10/BT.2100.
      state.dlssg_supported = format_kind == FormatKind::kRgb10A2Unorm;
      break;
    case kColorSpaceHdr10Hlg:
      state.encoding = Encoding::kHdrOther;
      state.hdr = true;
      state.wide_gamut = true;
      state.known = true;
      state.dlssg_supported = false;
      break;
    default:
      break;
  }
  return state;
}

inline bool SupportsHudSeparation(const State& state) {
  return state.known && state.dlssg_supported;
}

inline UiEncoding DetectedUiEncoding(FormatKind format_kind) {
  switch (format_kind) {
    case FormatKind::kRgba8Srgb:
      return UiEncoding::kSdrSrgb;
    case FormatKind::kRgba8Unorm:
      return UiEncoding::kSdrLinear;
    default:
      return UiEncoding::kUnknown;
  }
}

inline bool IsUiEncodingCompatible(const State& output, UiEncoding ui_encoding,
                                   bool native_streamline_tag) {
  if (!SupportsHudSeparation(output)) return false;
  if (ui_encoding == UiEncoding::kAlphaOnly) return true;
  if (ui_encoding == UiEncoding::kOutputEncoded) return native_streamline_tag;

  // Existing automatic UI discovery observes RGBA8 SDR render targets. Preserve
  // that path for SDR outputs, but never treat those pixels as HDR10/PQ data.
  if (output.encoding == Encoding::kSdrSrgb ||
      output.encoding == Encoding::kSdrLinear) {
    return ui_encoding == UiEncoding::kSdrSrgb ||
           ui_encoding == UiEncoding::kSdrLinear;
  }
  return false;
}

inline const char* EncodingName(Encoding encoding) {
  switch (encoding) {
    case Encoding::kSdrSrgb: return "SDR sRGB";
    case Encoding::kSdrLinear: return "SDR linear";
    case Encoding::kHdr10Pq: return "HDR10 PQ";
    case Encoding::kScRgbLinear: return "scRGB linear";
    case Encoding::kHdrOther: return "HDR other";
    default: return "unknown";
  }
}

inline const char* UiEncodingName(UiEncoding encoding) {
  switch (encoding) {
    case UiEncoding::kAlphaOnly: return "alpha-only";
    case UiEncoding::kSdrSrgb: return "SDR sRGB";
    case UiEncoding::kSdrLinear: return "SDR linear";
    case UiEncoding::kOutputEncoded: return "output-encoded";
    default: return "unknown";
  }
}

inline std::shared_mutex g_mutex;
inline State g_state{};

inline State Read() {
  std::shared_lock lock(g_mutex);
  return g_state;
}

inline bool Observe(FormatKind format_kind, uint32_t format, bool format_known,
                    uint32_t color_space, bool color_space_known) {
  State next = Classify(format_kind, format, format_known, color_space,
                        color_space_known);
  std::unique_lock lock(g_mutex);
  if (Equivalent(g_state, next)) return false;
  next.revision = g_state.revision + 1;
  g_state = next;
  return true;
}

inline bool Reset() {
  std::unique_lock lock(g_mutex);
  State next{};
  if (Equivalent(g_state, next)) return false;
  next.revision = g_state.revision + 1;
  g_state = next;
  return true;
}

}  // namespace mfgunlock::outputstate
