// SPDX-License-Identifier: MIT
// Production FilterUiTags with Vulkan metadata; no Vulkan GPU execution.
#include "framecount.hpp"
#include <cassert>
#include <cstdio>
namespace f=mfgunlock::framecount;
namespace o=mfgunlock::outputstate;
namespace q=mfgunlock::qualityguard;
unsigned forwarded=0; std::vector<sl::ResourceTag> observed;
sl::Result Forward(const sl::ViewportHandle&,const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c){
  assert(c==reinterpret_cast<sl::CommandBuffer*>(0xabc));++forwarded;
  observed.assign(t,t+n);return sl::Result::eOk;
}
sl::Resource Make(uint64_t image,uint32_t format){
  sl::Resource r{};r.native=reinterpret_cast<void*>(image);r.view=reinterpret_cast<void*>(image+0x100);
  r.width=128;r.height=72;r.nativeFormat=format;r.state=1;return r;
}
int main(){
  mfgunlock::g_enabled=true;f::g_ui_composition_enabled=true;f::g_ui_candidate_injection_enabled=false;
  f::internal::g_real_set_tag=Forward;o::SetResourceApi(o::ResourceApi::kVulkan);
  o::Observe(o::FormatKind::kRgb10A2Unorm,24,true,o::kColorSpaceHdr10Pq,true);
  auto color=Make(1,64),hudless=Make(2,64),ui=Make(3,37);
  sl::Extent extent{};extent.width=128;extent.height=72;
  sl::ResourceTag tags[]={
    {&ui,sl::kBufferTypeUIColorAndAlpha,sl::ResourceLifecycle::eValidUntilPresent,&extent},
    {&color,sl::kBufferTypeBackbuffer,sl::ResourceLifecycle::eValidUntilPresent,&extent},
    {&hudless,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent,&extent}};
  auto assessment=q::AssessTags(tags,3,o::Read());
  assert(q::CanAutomaticallyUseUiRecomposition(assessment,o::Read()));
  f::internal::HookedSetTag(sl::ViewportHandle(0),tags,3,reinterpret_cast<sl::CommandBuffer*>(0xabc));
  assert(forwarded==1&&observed.size()==3&&observed[0].resource==&ui&&observed[1].resource==&color);
  assert(ui.state==1&&ui.nativeFormat==37&&ui.view==reinterpret_cast<void*>(0x103));
  ui.view=nullptr;assessment=q::AssessTags(tags,3,o::Read());
  assert(!q::CanAutomaticallyUseUiRecomposition(assessment,o::Read())&&!assessment.suppress_hud_separation);
  f::internal::HookedSetTag(sl::ViewportHandle(0),tags,3,reinterpret_cast<sl::CommandBuffer*>(0xabc));
  assert(forwarded==2&&observed[0].resource==&ui); // Unknown metadata is not rewritten.
  ui=Make(3,64);assessment=q::AssessTags(tags,3,o::Read());
  assert(assessment.issues&q::kUiColorAlphaLowPrecision);
  ui=Make(3,24);assessment=q::AssessTags(tags,3,o::Read());
  assert(!(assessment.issues&q::kUiColorAlphaLowPrecision));
  assert(!q::CanAutomaticallyUseUiRecomposition(assessment,o::Read()));
  ui=Make(3,37);ui.state=0;assessment=q::AssessTags(tags,3,o::Read());
  assert(!q::CanAutomaticallyUseUiRecomposition(assessment,o::Read()));
  o::Observe(o::FormatKind::kRgba16Float,10,true,o::kColorSpaceScRgbLinear,true);
  assert(!q::CanAutomaticallyUseUiRecomposition(q::AssessTags(tags,3,o::Read()),o::Read()));
  // Invalid UI metadata in an earlier batch must not be "proven" by a later
  // valid HUDless-only batch. Native batches themselves remain unchanged.
  o::Observe(o::FormatKind::kRgb10A2Unorm,24,true,o::kColorSpaceHdr10Pq,true);
  f::internal::ForgetUiOutputs();
  ui=Make(3,37);ui.view=nullptr;
  f::internal::HookedSetTag(sl::ViewportHandle(0),tags,1,reinterpret_cast<sl::CommandBuffer*>(0xabc));
  f::internal::HookedSetTag(sl::ViewportHandle(0),tags+1,2,reinterpret_cast<sl::CommandBuffer*>(0xabc));
  assert(!f::g_ui_pair_eligible.load());
  ui=Make(3,37);
  f::internal::HookedSetTag(sl::ViewportHandle(0),tags,1,reinterpret_cast<sl::CommandBuffer*>(0xabc));
  assert(f::g_ui_pair_eligible.load());
  o::SetResourceApi(o::ResourceApi::kD3D12);
  assert(o::LowPrecisionUiAlpha(24)&&!o::LowPrecisionUiAlpha(64));
  puts("vulkan_ui_test PASS (real tag path: native ownership, format domain, HDR metadata)");
}
