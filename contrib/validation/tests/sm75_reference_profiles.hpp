// SPDX-License-Identifier: MIT
#pragma once
// Historical coverage evidence, never a runtime admission table.
namespace mfgunlock::ptx {
struct PackedHalfProfile {
  size_t bytes;
  uint64_t fingerprint;
  size_t conversions;
};

// The only inspected SM89 packed-half programs in DLSS-G 310.9.1.
inline constexpr PackedHalfProfile kPackedHalfProfiles[] = {
    {42029, 0x9b5219cdd66e0969ull, 20},  // BlendCandidatesFused
    {10036, 0x87e83571b0c08cbeull, 1},   // DL1Net_Input
    {30175, 0x6578876be2b5fbe2ull, 8},   // OutputPull
    {11165, 0x1d6ba2d7fd03152eull, 8},   // OutputPullMiddle
    {52834, 0x62a7b5ee15c6f3e1ull, 2},   // OutputPush
};

inline size_t PackedHalfCount(std::string_view source) {
  const auto hash = SourceFingerprint(source);
  for (const auto& profile : kPackedHalfProfiles) {
    if (source.size() == profile.bytes && hash == profile.fingerprint)
      return profile.conversions;
  }
  return 0;
}


struct TuringMmaProfile {
  size_t bytes;
  uint64_t fingerprint;
  size_t instructions;
  HalfMinMaxCounts half_minmax;
};

// Exact single-image SM89 programs in DLSS-G 310.9.1 that use the Ampere
// m16n8k16 f16 MMA shape. Unknown programs remain rejected.
inline constexpr TuringMmaProfile kTuringMmaProfiles[] = {
    {20437, 0xce2c5c3f5d55cd81ull, 18, {4, 4, 0, 0}},
    {11638, 0x75fe3a8642b3e477ull, 9, {0, 0, 4, 2}},
    {16101, 0xd932cb22dd83419eull, 18, {0, 0, 4, 2}},
    {15662, 0x6702b594af03caecull, 18, {0, 0, 4, 2}},
    {16341, 0x8eb26063a973182cull, 12, {0, 0, 4, 2}},
    {13231, 0xbc841338134753c6ull, 12, {0, 0, 4, 2}},
    {14603, 0xdde1c125651eb889ull, 9, {0, 0, 4, 2}},
    {11750, 0xfdfe9b7351889bfdull, 9, {0, 0, 4, 2}},
    {14684, 0xc83bdce19a1a249full, 18, {0, 0, 4, 2}},
    {14693, 0x400b19b43610cce1ull, 18, {0, 0, 4, 2}},
    {17609, 0xe0ba6d48fb005cc1ull, 9, {4, 4, 0, 0}},
    {33114, 0xa582db83fc911fe0ull, 32, {8, 0, 0, 0}},
    {16453, 0xfb821a89f2997eceull, 16, {16, 0, 0, 0}},
    {32740, 0x802242e77fae47cbull, 36, {16, 0, 0, 0}},
    {12511, 0xe52f68bf382c1392ull, 8, {16, 0, 0, 0}},
    {9881, 0xdba2cda2f6af70a5ull, 4, {0, 0, 0, 0}},
    {56261, 0x80fccb8da5f37636ull, 108, {48, 0, 0, 0}},
    {12754, 0xe1fd0775658c8dd3ull, 8, {16, 0, 0, 0}},
    {59794, 0x58f45c0876b5b7c5ull, 64, {8, 0, 0, 0}},
    {33295, 0x3e454577f62ecf8full, 36, {16, 0, 0, 0}},
    {43831, 0x6e407251363cf2a5ull, 48, {8, 0, 0, 0}},
    {33114, 0x625ec319e7d0a5c9ull, 32, {8, 0, 0, 0}},
    {33114, 0xe5fa510399d45224ull, 32, {8, 0, 0, 0}},
    {20153, 0xdef932bb960c28d0ull, 16, {8, 0, 0, 0}},
    {24116, 0x5702cf69c73d6af4ull, 32, {16, 0, 0, 0}},
    {24206, 0xdc473e258540ef53ull, 32, {16, 0, 0, 0}},
    {16214, 0x92f1a598e2b2254cull, 16, {16, 0, 0, 0}},
};

inline const TuringMmaProfile* FindTuringMmaProfile(std::string_view source) {
  const auto hash = SourceFingerprint(source);
  for (const auto& profile : kTuringMmaProfiles) {
    if (source.size() == profile.bytes && hash == profile.fingerprint) return &profile;
  }
  return nullptr;
}

inline size_t TuringMmaCount(std::string_view source) {
  const auto* profile = FindTuringMmaProfile(source);
  return profile == nullptr ? 0 : profile->instructions;
}

}
