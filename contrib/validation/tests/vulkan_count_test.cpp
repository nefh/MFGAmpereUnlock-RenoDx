// SPDX-License-Identifier: MIT
#include "framecount.hpp"
#include <cassert>
#include <cstdio>
namespace f=mfgunlock::framecount;namespace o=mfgunlock::outputstate;
unsigned pacing=0,sets=0,states=0,native_max=5,received=0;
sl::Result result=sl::Result::eOk;
sl::DLSSGOptions last;
sl::Result Set(const sl::ViewportHandle&,const sl::DLSSGOptions& in){++sets;received=in.numFramesToGenerate;last=in;return result;}
sl::Result State(const sl::ViewportHandle&,sl::DLSSGState& out,const sl::DLSSGOptions*){
  ++states;out.numFramesToGenerateMax=native_max;out.status=0;return result;
}
sl::Result Init(const sl::Preferences&,uint64_t){return result;}
int main(){
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);mfgunlock::g_enabled=true;
  f::internal::g_real_set_options=Set;f::internal::g_real_get_state=State;f::internal::g_real_init=Init;
  f::g_multi_frame_ready=[] {return true;};f::g_provider_mfg_ceiling=[] {return 5u;};
  f::g_streamline_plugin_seen=true;f::g_streamline_max_generated=5;
  f::g_ensure_pacing=[] {++pacing;};f::g_pacing_ready=[] {return false;};
  f::g_ui_composition_enabled=false;f::g_dynamic_mfg_enabled=false;f::g_force_multiplier=0;
  sl::Preferences pref{};pref.renderAPI=sl::RenderAPI::eVulkan;
  f::internal::HookedInit(pref,0);assert(o::GetResourceApi()==o::ResourceApi::kVulkan);
  sl::DLSSGOptions options{};options.structVersion=sl::kStructVersion5;
  options.mode=sl::DLSSGMode::eOn;options.numFramesToGenerate=3;options.queueParallelismMode=1;
  options.onErrorCallback=reinterpret_cast<void*>(0x345);options.next=reinterpret_cast<void*>(0x456);
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);
  assert(received==3&&sets==1&&pacing==0);
  assert(last.queueParallelismMode==1&&last.onErrorCallback==options.onErrorCallback&&last.next==options.next);
  f::g_force_multiplier=4;options.numFramesToGenerate=1;
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==1&&pacing==0);
  sl::DLSSGState state{};state.structVersion=sl::kStructVersion4;
  f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);assert(state.numFramesToGenerateMax==5&&states==1);
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==3&&pacing==0);
  f::internal::HookedSetOptions(sl::ViewportHandle(1),options);assert(received==1);
  f::internal::InvalidateLiveOptions();f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==1);
  native_max=1;f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==1); // Structural ceiling MUST NOT fabricate native Vulkan max.
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==1);
  native_max=5;f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  result=sl::Result::eErrorUnsupportedInterface;f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  result=sl::Result::eOk;f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==1);
  o::SetResourceApi(o::ResourceApi::kD3D12);options.numFramesToGenerate=3;f::g_force_multiplier=0;
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==1&&pacing>0);
  puts("vulkan_count_test PASS (real SetOptions/GetState; host queue mode preserved; no D3D pacer)");
}
