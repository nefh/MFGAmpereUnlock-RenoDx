// SPDX-License-Identifier: MIT
#pragma once
#include "sl.h"
namespace sl {
constexpr uint32_t kStructVersion2 = 2;
constexpr uint32_t kStructVersion3 = 3;
constexpr uint32_t kStructVersion4 = 4;
constexpr uint32_t kStructVersion5 = 5;
enum class Boolean : uint32_t { eFalse = 0, eTrue = 1 };
enum class DLSSGMode : uint32_t { eOff, eOn, eDynamic };
struct DLSSGOptions {
  void* next = nullptr;
  uint32_t structVersion = kStructVersion1;
  DLSSGMode mode = DLSSGMode::eOn;
  uint32_t numFramesToGenerate = 1;
  uint32_t flags = 0;
  uint32_t dynamicResWidth = 0;
  uint32_t dynamicResHeight = 0;
  uint32_t numBackBuffers = 0;
  uint32_t mvecDepthWidth = 0;
  uint32_t mvecDepthHeight = 0;
  uint32_t colorWidth = 0;
  uint32_t colorHeight = 0;
  uint32_t colorBufferFormat = 0;
  uint32_t mvecBufferFormat = 0;
  uint32_t depthBufferFormat = 0;
  uint32_t hudLessBufferFormat = 0;
  uint32_t uiBufferFormat = 0;
  void* onErrorCallback = nullptr;
  uint32_t bReserved15 = 0;
  uint32_t queueParallelismMode = 0;
  Boolean enableUserInterfaceRecomposition = Boolean::eFalse;
  float dynamicTargetFrameRate = 0.0f;
};
struct DLSSGState {
  void* next = nullptr;
  uint32_t structVersion = kStructVersion2;
  uint32_t status = 0;
  uint32_t numFramesActuallyPresented = 0;
  uint32_t numFramesToGenerateMax = 1;
  Boolean bIsDynamicMFGSupported = Boolean::eFalse;
};
}  // namespace sl
