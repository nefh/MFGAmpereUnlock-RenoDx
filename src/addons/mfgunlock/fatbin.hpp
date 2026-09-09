/*
 * Minimal CUDA fatbin parser used by the Ampere PTX retargeter.
 * SPDX-License-Identifier: MIT
 *
 * NVIDIA does not publish this container format as a stable ABI. Every parser
 * check below is therefore deliberately conservative: unknown headers, entry
 * layouts, compression flags, or sizes are rejected rather than guessed.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace mfgunlock::fatbin {

inline constexpr uint32_t kMagic = 0xBA55ED50u;
inline constexpr size_t kHeaderBytes = 16;
inline constexpr size_t kMaxPtxBytes = 32 * 1024 * 1024;
inline constexpr size_t kMaxFatbinBytes = 64 * 1024 * 1024;

inline uint16_t ReadU16(const unsigned char* data) {
  uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

inline uint32_t ReadU32(const unsigned char* data) {
  uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

inline uint64_t ReadU64(const unsigned char* data) {
  uint64_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

// Plain LZ4 block format. The optional mapping identifies the source literal
// responsible for one decoded byte. A second full decode is still required to
// prove that editing that literal does not affect any other output position.
inline bool Lz4BlockDecompress(const unsigned char* src, size_t src_bytes,
                               unsigned char* dst, size_t dst_bytes,
                               size_t wanted = std::numeric_limits<size_t>::max(),
                               size_t* literal = nullptr) {
  size_t in = 0;
  size_t out = 0;

  if (literal != nullptr) *literal = std::numeric_limits<size_t>::max();

  auto extend = [&](size_t& length) {
    if (length != 15) return true;

    unsigned int extra = 0;
    do {
      if (in == src_bytes) return false;
      extra = src[in++];
      if (length > dst_bytes || extra > dst_bytes - length) return false;
      length += extra;
    } while (extra == 255);

    return true;
  };

  while (in < src_bytes) {
    const unsigned int token = src[in++];
    size_t length = token >> 4;
    if (!extend(length) || length > src_bytes - in || length > dst_bytes - out) return false;

    if (literal != nullptr && wanted >= out && wanted - out < length) {
      *literal = in + wanted - out;
    }

    if (length != 0) std::memcpy(dst + out, src + in, length);
    in += length;
    out += length;

    if (in == src_bytes) return out == dst_bytes;
    if (src_bytes - in < 2) return false;

    const size_t distance = ReadU16(src + in);
    in += 2;
    if (distance == 0 || distance > out) return false;

    length = token & 15;
    if (!extend(length) || length > std::numeric_limits<size_t>::max() - 4) return false;
    length += 4;
    if (length > dst_bytes - out) return false;

    // LZ4 match copies may overlap. A byte-at-a-time copy is intentional here;
    // memcpy and memmove do not have the same semantics for this format.
    for (size_t i = 0; i < length; ++i) {
      if (i >= dst_bytes - out) return false;
      const size_t dst_index = out + i;
      if (distance > dst_index) return false;
      dst[dst_index] = dst[dst_index - distance];
    }
    out += length;
  }

  return out == dst_bytes;
}

struct Entry {
  size_t offset;
  size_t header_bytes;
  size_t payload_bytes;
  size_t compressed_bytes;
  uint16_t kind;
  uint32_t architecture;
  uint64_t flags;
  size_t unpacked_bytes;

  size_t PayloadOffset() const {
    return offset + header_bytes;
  }

  size_t End() const {
    return PayloadOffset() + payload_bytes;
  }
};

inline bool Parse(std::span<const unsigned char> bytes, std::vector<Entry>& entries,
                  size_t& end) {
  entries.clear();

  if (bytes.size() < kHeaderBytes || bytes.size() > kMaxFatbinBytes ||
      ReadU32(bytes.data()) != kMagic || ReadU16(bytes.data() + 4) != 1 ||
      ReadU16(bytes.data() + 6) != kHeaderBytes) {
    return false;
  }

  const uint64_t payload_bytes = ReadU64(bytes.data() + 8);
  if (payload_bytes > bytes.size() - kHeaderBytes) return false;

  end = kHeaderBytes + static_cast<size_t>(payload_bytes);
  size_t offset = kHeaderBytes;

  while (offset < end) {
    if (end - offset < 64 || entries.size() == 32) return false;

    const auto* entry = bytes.data() + offset;
    const size_t header_bytes = ReadU32(entry + 4);
    const uint64_t stored_payload_bytes = ReadU64(entry + 8);
    const uint32_t compressed_bytes = ReadU32(entry + 16);
    const uint64_t unpacked_bytes = ReadU64(entry + 56);
    const uint16_t kind = ReadU16(entry);

    if ((kind != 1 && kind != 2) || ReadU16(entry + 2) != 0x101 ||
        header_bytes < 64 || header_bytes > 4096 || header_bytes % 8 != 0 ||
        stored_payload_bytes % 8 != 0 || header_bytes > end - offset ||
        stored_payload_bytes > end - offset - header_bytes ||
        compressed_bytes > stored_payload_bytes || unpacked_bytes > kMaxPtxBytes) {
      return false;
    }

    entries.push_back({
        offset,
        header_bytes,
        static_cast<size_t>(stored_payload_bytes),
        compressed_bytes,
        kind,
        ReadU32(entry + 28),
        ReadU64(entry + 40),
        static_cast<size_t>(unpacked_bytes),
    });
    offset = entries.back().End();
  }

  return offset == end && !entries.empty();
}

}  // namespace mfgunlock::fatbin
