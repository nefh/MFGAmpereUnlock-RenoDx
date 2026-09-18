// SPDX-License-Identifier: MIT
#pragma once
#include "sl.h"
namespace sl {
constexpr uint32_t kStructVersion2 = 2;
constexpr uint32_t kStructVersion3 = 3;
constexpr uint32_t kStructVersion4 = 4;
constexpr uint32_t kStructVersion5 = 5;
enum class Boolean : uint32_t { eFalse = 0, eTrue = 1 };
using BufferType = uint32_t;
constexpr BufferType kBufferTypeHUDLessColor = 2;
constexpr BufferType kBufferTypeUIColorAndAlpha = 23;
constexpr BufferType kBufferTypeBackbuffer = 53;
constexpr BufferType kBufferTypeUIAlpha = 69;
enum class ResourceLifecycle : uint32_t {
  eOnlyValidNow,
  eValidUntilPresent,
  eValidUntilEvaluate,
};
enum class ResourceType : uint32_t { eTex2d = 1 };
using CommandBuffer = void;
using FrameToken = uint32_t;
struct Extent {
  uint32_t top = 0;
  uint32_t left = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};
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
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t nativeFormat = 0;

  Resource() = default;
  Resource(ResourceType type_value, void* native_value, uint32_t state_value)
      : type(type_value), native(native_value), state(state_value) {}
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

  ResourceTag() = default;
  ResourceTag(Resource* resource_value, BufferType type_value,
              ResourceLifecycle lifecycle_value, const Extent* extent_value = nullptr)
      : resource(resource_value), type(type_value), lifecycle(lifecycle_value) {
    if (extent_value != nullptr) extent = *extent_value;
  }
};
struct Constants {
  static constexpr uint32_t s_structType = 0x636f6e73;
  uint32_t structType = s_structType;
  uint32_t structVersion = kStructVersion2;
  Boolean reset = Boolean::eFalse;
  float minRelativeLinearDepthObjectSeparation = 40.0f;
};
enum class DLSSGMode : uint32_t { eOff, eOn, eDynamic };
struct DLSSGOptions {
  static constexpr uint32_t s_structType = 0x646c7367;
  void* next = nullptr;
  uint32_t structType = s_structType;
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
  static constexpr uint32_t s_structType = 0x646c7374;
  uint32_t structType = s_structType;
  void* next = nullptr;
  uint32_t structVersion = kStructVersion2;
  uint32_t status = 0;
  uint32_t numFramesActuallyPresented = 0;
  uint32_t numFramesToGenerateMax = 1;
  Boolean bIsDynamicMFGSupported = Boolean::eFalse;
};
}  // namespace sl
using PFun_slSetTagForFrame = sl::Result(
    const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*,
    uint32_t, sl::CommandBuffer*);
using PFun_slSetConstants = sl::Result(
    const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
