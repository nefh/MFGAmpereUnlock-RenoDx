// SPDX-License-Identifier: MIT
#pragma once
#include "sl.h"
namespace sl {
constexpr uint32_t kStructVersion1 = 1;
constexpr uint32_t kStructVersion2 = 2;
constexpr uint32_t kStructVersion3 = 3;
constexpr uint32_t kStructVersion4 = 4;
constexpr uint32_t kStructVersion5 = 5;
enum class Boolean : uint32_t { eFalse = 0, eTrue = 1 };
struct float2 { float x = 0, y = 0; };
struct float3 { float x = 0, y = 0, z = 0; };
struct float4 { float x = 0, y = 0, z = 0, w = 0; };
struct float4x4 { float4 row[4]{}; };
using BufferType = uint32_t;
enum class ResourceLifecycle : uint32_t { eOnlyValidNow, eValidUntilPresent, eValidUntilEvaluate };
enum class ResourceType : uint32_t { eTex2d = 1 };
using CommandBuffer = void;
struct Extent { uint32_t top = 0, left = 0, width = 0, height = 0; };
struct Resource {
  static constexpr uint32_t s_structType = 0x7265736f;
  uint32_t structType = s_structType;
  uint32_t structVersion = kStructVersion1;
  void* next = nullptr;
  ResourceType type = ResourceType::eTex2d;
  void* native = nullptr;
  void* memory = nullptr;
  void* view = nullptr;
  uint32_t state = UINT32_MAX;
  uint32_t width = 0, height = 0, nativeFormat = 0;
};
struct ResourceTag {
  static constexpr uint32_t s_structType = 0x74616767;
  uint32_t structType = s_structType;
  uint32_t structVersion = kStructVersion1;
  void* next = nullptr;
  Resource* resource = nullptr;
  BufferType type = 0;
  ResourceLifecycle lifecycle = ResourceLifecycle::eValidUntilPresent;
  Extent extent{};
};
enum class DLSSGMode : uint32_t { eOff = 0, eOn = 1, eDynamic = 2 };
struct DLSSGOptions {
  static constexpr uint32_t s_structType = 0x646c7367;
  void* next = nullptr;
  uint32_t structType = s_structType;
  uint32_t structVersion = kStructVersion5;
  DLSSGMode mode = DLSSGMode::eOn;
  uint32_t numFramesToGenerate = 1;
  uint32_t flags = 0;
  uint32_t dynamicResWidth = 0, dynamicResHeight = 0, numBackBuffers = 0;
  uint32_t mvecDepthWidth = 0, mvecDepthHeight = 0, colorWidth = 0, colorHeight = 0;
  uint32_t colorBufferFormat = 0, mvecBufferFormat = 0, depthBufferFormat = 0;
  uint32_t hudLessBufferFormat = 0, uiBufferFormat = 0;
  uint32_t queueParallelismMode = 0;
  Boolean enableUserInterfaceRecomposition = Boolean::eFalse;
  float dynamicTargetFrameRate = 0;
};
struct Constants {
  static constexpr uint32_t s_structType = 0x636f6e73;
  uint32_t structType = s_structType;
  uint32_t structVersion = kStructVersion2;
  void* next = nullptr;
  float4x4 cameraViewToClip{}, clipToCameraView{}, clipToLensClip{}, clipToPrevClip{}, prevClipToClip{};
  float2 jitterOffset{}, mvecScale{}, cameraPinholeOffset{};
  float3 cameraPos{}, cameraUp{}, cameraRight{}, cameraFwd{};
  float cameraNear = 0, cameraFar = 0, cameraFOV = 0, cameraAspectRatio = 0;
  float motionVectorsInvalidValue = 0, minRelativeLinearDepthObjectSeparation = 0;
  Boolean depthInverted = Boolean::eFalse;
  Boolean cameraMotionIncluded = Boolean::eFalse;
  Boolean motionVectors3D = Boolean::eFalse;
  Boolean reset = Boolean::eFalse;
  Boolean orthographicProjection = Boolean::eFalse;
  Boolean motionVectorsDilated = Boolean::eFalse;
  Boolean motionVectorsJittered = Boolean::eFalse;
};
struct DLSSGState {
  static constexpr uint32_t s_structType = 0x646c7374;
  uint32_t structType = s_structType;
  uint32_t structVersion = kStructVersion4;
  uint32_t status = 0;
  uint32_t numFramesActuallyPresented = 0;
  uint32_t numFramesToGenerateMax = 1;
  Boolean bIsVsyncSupportAvailable = Boolean::eFalse;
  Boolean bIsDynamicMFGSupported = Boolean::eFalse;
};
}
