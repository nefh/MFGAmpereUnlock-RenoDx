// SPDX-License-Identifier: MIT
#pragma once
#include "sl.h"
namespace sl {
enum class PCLMarker : uint32_t {
  eSimulationStart = 0,
  eSimulationEnd = 1,
  eRenderSubmitStart = 2,
  eRenderSubmitEnd = 3,
  ePresentStart = 4,
  ePresentEnd = 5,
};
}
using PFun_slPCLSetMarker = sl::Result(sl::PCLMarker, const sl::FrameToken&);
