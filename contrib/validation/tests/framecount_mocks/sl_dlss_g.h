// SPDX-License-Identifier: MIT
#pragma once
#include "sl.h"
namespace sl {
constexpr uint32_t kStructVersion2 = 2;
enum class DLSSGMode { eOff, eOn };
struct DLSSGOptions { DLSSGMode mode = DLSSGMode::eOn; uint32_t numFramesToGenerate = 1; };
struct DLSSGState {
  uint32_t structVersion = kStructVersion2;
  uint32_t status = 0;
  uint32_t numFramesActuallyPresented = 0;
  uint32_t numFramesToGenerateMax = 1;
};
}  // namespace sl
