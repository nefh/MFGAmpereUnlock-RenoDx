// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <type_traits>

namespace mfgunlock::vulkancontract {
// VkFormat numbers belong to a DIFFERENT namespace than ReShade/DXGI formats.
// No version/image identity appears in this contract.
enum class FormatKind : uint32_t { kUnknown, kRgba8, kBgra8, kRgb10A2, kRgba16Float, kOther };
inline FormatKind ClassifyFormat(uint32_t format) {
  switch (format) {
    case 0: return FormatKind::kUnknown;
    case 37: case 43: return FormatKind::kRgba8;
    case 44: case 50: return FormatKind::kBgra8;
    case 58: case 64: return FormatKind::kRgb10A2;
    case 97: return FormatKind::kRgba16Float;
    default: return FormatKind::kOther;
  }
}
inline bool LowPrecisionColorAlpha(uint32_t format) {
  return ClassifyFormat(format) == FormatKind::kRgb10A2;
}
inline bool DefinedLayout(uint32_t layout) {
  // Unknown, undefined and preinitialized are not usable contents. Other
  // extension layouts remain opaque/native; this is NOT an ownership proof.
  return layout != 0 && layout != 8 && layout != UINT32_MAX;
}
template<class T> inline uint64_t HandleBits(T value) {
  if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uint64_t>(value);
  else return static_cast<uint64_t>(value);
}
struct Image {
  uint64_t image = 0, view = 0;
  uint32_t format = 0, width = 0, height = 0;
  uint32_t aspect = 0, first_mip = 0, mip_count = 0, first_layer = 0, layer_count = 0;
  bool writable = false;
};
inline bool FiniteRange(uint32_t first, uint32_t count) {
  return count != 0 && count != UINT32_MAX && first < UINT32_MAX &&
      uint64_t{first} + count <= UINT32_MAX;
}
inline bool ImageMetadataValid(const Image& image) {
  return image.image && image.view && image.format && image.width && image.height &&
      image.aspect && !(image.aspect & ~7u) &&
      FiniteRange(image.first_mip, image.mip_count) &&
      FiniteRange(image.first_layer, image.layer_count);
}
// NGX's public Resource_VK does not carry layout, queue ownership, semaphore,
// completion or a transferable image reference. Never manufacture those facts.
} // namespace mfgunlock::vulkancontract
