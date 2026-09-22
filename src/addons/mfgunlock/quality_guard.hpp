/*
 * Conservative Streamline DLSS-G input quality guard.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sl.h>
#include <sl_dlss_g.h>

#include "./output_state.hpp"

namespace mfgunlock::qualityguard {

enum Issue : uint32_t {
  kNone = 0,
  kHdrFinalColorIsolation = 1u << 0,
  kInvalidOptionalResource = 1u << 1,
  kHudlessExtentMismatch = 1u << 2,
  kHudlessFormatMismatch = 1u << 3,
  kUiExtentMismatch = 1u << 4,
  kUiColorAlphaLowPrecision = 1u << 5,
  kOutputEncodingUnknown = 1u << 6,
  kOutputEncodingUnsupported = 1u << 7,
  kUiEncodingMismatch = 1u << 8,
};

enum class SuppressionPolicy : uint32_t {
  kNone = 0,
  kUiOnly = 1,
  kAllHudSeparation = 2,
};

struct OutputDescription {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;

  [[nodiscard]] bool HasDimensions() const { return width != 0 && height != 0; }
  [[nodiscard]] bool HasFormat() const { return format != 0; }
};

struct Assessment {
  uint32_t issues = kNone;
  bool has_hud_separation = false;
  bool has_hudless_color = false;
  bool has_ui_color_or_alpha = false;
  bool has_ui_color_alpha = false;
  bool has_ui_alpha = false;
  bool clears_hudless_color = false;
  bool clears_ui_color_or_alpha = false;
  bool clears_ui_color_alpha = false;
  bool clears_ui_alpha = false;
  bool suppress_hud_separation = false;
  outputstate::Encoding render_encoding = outputstate::Encoding::kUnknown;
  bool render_encoding_known = false;
  outputstate::UiEncoding ui_encoding = outputstate::UiEncoding::kUnknown;
  OutputDescription observed_backbuffer{};
};

inline bool IsUiColorOrAlphaType(sl::BufferType type) {
  return type == sl::kBufferTypeUIColorAndAlpha ||
         type == sl::kBufferTypeUIAlpha;
}

inline bool IsHudSeparationType(sl::BufferType type) {
  return type == sl::kBufferTypeHUDLessColor || IsUiColorOrAlphaType(type);
}

inline bool HasStructuralIssues(const Assessment& assessment) {
  constexpr uint32_t kStructuralIssues =
      kInvalidOptionalResource | kHudlessExtentMismatch |
      kHudlessFormatMismatch | kUiExtentMismatch |
      kUiColorAlphaLowPrecision;
  return (assessment.issues & kStructuralIssues) != 0;
}

inline OutputDescription Describe(const sl::ResourceTag& tag, bool* valid = nullptr) {
  const bool lifecycle_valid =
      tag.lifecycle == sl::ResourceLifecycle::eOnlyValidNow ||
      tag.lifecycle == sl::ResourceLifecycle::eValidUntilPresent ||
      tag.lifecycle == sl::ResourceLifecycle::eValidUntilEvaluate;
  bool local_valid = tag.structType == sl::ResourceTag::s_structType &&
                     tag.structVersion == sl::kStructVersion1 && lifecycle_valid;
  OutputDescription result{};

  if (tag.extent.width != 0 && tag.extent.height != 0) {
    result.width = tag.extent.width;
    result.height = tag.extent.height;
  }

  if (tag.resource != nullptr) {
    local_valid = local_valid &&
                  tag.resource->structType == sl::Resource::s_structType &&
                  tag.resource->structVersion == sl::kStructVersion1;
    if (!result.HasDimensions()) {
      result.width = tag.resource->width;
      result.height = tag.resource->height;
    }
    result.format = tag.resource->nativeFormat;

    if (tag.extent.width != 0 && tag.extent.height != 0 &&
        tag.resource->width != 0 && tag.resource->height != 0) {
      const uint64_t right = static_cast<uint64_t>(tag.extent.left) + tag.extent.width;
      const uint64_t bottom = static_cast<uint64_t>(tag.extent.top) + tag.extent.height;
      local_valid = local_valid && right <= tag.resource->width &&
                    bottom <= tag.resource->height;
    }
  }

  if (valid != nullptr) *valid = local_valid;
  return result;
}

inline Assessment AssessTags(const sl::ResourceTag* tags, uint32_t count,
                             const outputstate::State& output_state,
                             const OutputDescription& previous_output = {}) {
  Assessment result{};
  if (tags == nullptr || count == 0) return result;

  OutputDescription output = previous_output;
  for (uint32_t index = 0; index < count; ++index) {
    const auto& tag = tags[index];
    if (tag.structVersion != sl::kStructVersion1 ||
        tag.type != sl::kBufferTypeBackbuffer) {
      continue;
    }
    bool valid = false;
    const auto current = Describe(tag, &valid);
    if (!valid) continue;
    if (current.HasDimensions()) {
      output.width = current.width;
      output.height = current.height;
    }
    if (current.HasFormat()) output.format = current.format;
  }
  result.observed_backbuffer = output;

  for (uint32_t index = 0; index < count; ++index) {
    const auto& tag = tags[index];
    if (!IsHudSeparationType(tag.type)) continue;

    result.has_hud_separation = true;
    if (tag.resource == nullptr) {
      result.clears_hudless_color |= tag.type == sl::kBufferTypeHUDLessColor;
      result.clears_ui_color_alpha |= tag.type == sl::kBufferTypeUIColorAndAlpha;
      result.clears_ui_alpha |= tag.type == sl::kBufferTypeUIAlpha;
      result.clears_ui_color_or_alpha |=
          result.clears_ui_color_alpha || result.clears_ui_alpha;
      continue;
    }

    result.has_hudless_color |= tag.type == sl::kBufferTypeHUDLessColor;
    result.has_ui_color_alpha |= tag.type == sl::kBufferTypeUIColorAndAlpha;
    result.has_ui_alpha |= tag.type == sl::kBufferTypeUIAlpha;
    result.has_ui_color_or_alpha |= result.has_ui_color_alpha || result.has_ui_alpha;

    bool valid = false;
    const auto resource = Describe(tag, &valid);
    if (!valid) result.issues |= kInvalidOptionalResource;
    if (resource.HasDimensions() && output.HasDimensions() &&
        (resource.width != output.width || resource.height != output.height)) {
      result.issues |= tag.type == sl::kBufferTypeHUDLessColor
                           ? kHudlessExtentMismatch
                           : kUiExtentMismatch;
    }
    if (tag.type == sl::kBufferTypeHUDLessColor && resource.HasFormat() &&
        output.HasFormat() && resource.format != output.format) {
      result.issues |= kHudlessFormatMismatch;
    }
    // DXGI_FORMAT_R10G10B10A2_UNORM has only two alpha bits and is not a
    // sufficiently precise UI Color & Alpha separation mask.
    if (tag.type == sl::kBufferTypeUIColorAndAlpha && resource.format == 24) {
      result.issues |= kUiColorAlphaLowPrecision;
    }
  }

  if (result.has_hudless_color && output_state.known) {
    // Native Streamline Hudless is application-provided and is documented to
    // use the same color space/post-processing domain as the color backbuffer.
    result.render_encoding = output_state.encoding;
    result.render_encoding_known = true;
  }

  if (result.has_ui_alpha) {
    result.ui_encoding = outputstate::UiEncoding::kAlphaOnly;
  } else if (result.has_ui_color_alpha) {
    // A native Streamline UI Color+Alpha tag is application-owned. The documented
    // contract requires it to reconstruct FinalColor from Hudless in the output
    // composition domain, so preserve that provenance rather than guessing from
    // the resource format alone.
    result.ui_encoding = outputstate::UiEncoding::kOutputEncoded;
  }

  if (result.has_hud_separation) {
    if (!output_state.known) {
      result.issues |= kOutputEncodingUnknown;
    } else if (!output_state.dlssg_supported) {
      result.issues |= kOutputEncodingUnsupported;
    } else if (result.has_ui_color_or_alpha &&
               !outputstate::IsUiEncodingCompatible(
                   output_state, result.ui_encoding, true)) {
      result.issues |= kUiEncodingMismatch;
    }
  }

  // Only malformed optional resources are rewritten. Unknown/unsupported output
  // encodings remain observational and leave native game tags untouched.
  result.suppress_hud_separation =
      result.has_hud_separation && HasStructuralIssues(result);
  return result;
}

inline outputstate::UiEncoding NativeUiEncoding(const Assessment& assessment) {
  if (assessment.has_ui_alpha) return outputstate::UiEncoding::kAlphaOnly;
  if (assessment.has_ui_color_alpha)
    return outputstate::UiEncoding::kOutputEncoded;
  return outputstate::UiEncoding::kUnknown;
}

inline bool IsStructurallyValidForUiRecomposition(const Assessment& assessment) {
  return assessment.has_hudless_color && assessment.has_ui_color_or_alpha &&
         !HasStructuralIssues(assessment);
}

inline bool CanAutomaticallyUseUiRecomposition(
    const Assessment& assessment, const outputstate::State& output_state) {
  return outputstate::SupportsHudSeparation(output_state) &&
         IsStructurallyValidForUiRecomposition(assessment) &&
         outputstate::IsUiEncodingCompatible(
             output_state, NativeUiEncoding(assessment), true);
}

inline SuppressionPolicy ResolveSuppression(
    const Assessment& current, const Assessment& accumulated,
    const outputstate::State& output_state, bool was_recomposition_eligible) {
  if (!current.has_hud_separation) return SuppressionPolicy::kNone;
  if (HasStructuralIssues(accumulated))
    return SuppressionPolicy::kAllHudSeparation;

  // If the output domain is unknown or unsupported, do not rewrite a native
  // game's optional tags. The addon simply declines automatic UIR.
  if (!outputstate::SupportsHudSeparation(output_state))
    return SuppressionPolicy::kNone;

  const bool eligible =
      CanAutomaticallyUseUiRecomposition(accumulated, output_state);
  const bool complete_current_pair =
      current.has_hudless_color && current.has_ui_color_or_alpha;
  if (eligible) {
    if (!was_recomposition_eligible && !complete_current_pair &&
        current.has_ui_color_or_alpha) {
      return SuppressionPolicy::kUiOnly;
    }
    return SuppressionPolicy::kNone;
  }

  return current.has_ui_color_or_alpha ? SuppressionPolicy::kUiOnly
                                       : SuppressionPolicy::kNone;
}

inline bool ShouldRequestAutomaticUiPath(const outputstate::State& output_state) {
  return outputstate::SupportsHudSeparation(output_state);
}

inline size_t ConstantsCopySize(const sl::Constants& source) {
  if (source.structType != sl::Constants::s_structType) return 0;
  if (source.structVersion == sl::kStructVersion1)
    return offsetof(sl::Constants, minRelativeLinearDepthObjectSeparation);
  if (source.structVersion == sl::kStructVersion2) return sizeof(sl::Constants);
  return 0;
}

inline bool CopyConstantsWithReset(const sl::Constants& source, void* destination,
                                   size_t capacity) {
  const size_t bytes = ConstantsCopySize(source);
  if (bytes == 0 || destination == nullptr || capacity < bytes) return false;
  std::memcpy(destination, &source, bytes);
  reinterpret_cast<sl::Constants*>(destination)->reset = sl::Boolean::eTrue;
  return true;
}

}  // namespace mfgunlock::qualityguard
