// SPDX-License-Identifier: MIT
// Native Vulkan API owns acceptance and pacing. Scan estimates do not veto calls.
#include "framecount.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
namespace f=mfgunlock::framecount;namespace o=mfgunlock::outputstate;
unsigned pacing=0,sets=0,states=0,native_max=5,received=0;
sl::Result result=sl::Result::eOk;
std::vector<unsigned> requests;
bool reject_multi=false;
unsigned advertised_max=5;
sl::DLSSGOptions last;
sl::Result Set(const sl::ViewportHandle&,const sl::DLSSGOptions& in){
  ++sets; received=in.numFramesToGenerate; requests.push_back(received); last=in;
  return reject_multi && received>1 ? sl::Result::eErrorUnsupportedInterface : result;
}
sl::Result State(const sl::ViewportHandle&,sl::DLSSGState& out,const sl::DLSSGOptions*){
  ++states;out.numFramesToGenerateMax=native_max;out.status=0;out.numFramesActuallyPresented=2;return result;
}
sl::Result Init(const sl::Preferences&,uint64_t){return result;}
int main(){
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);mfgunlock::g_enabled=true;
  f::internal::g_real_set_options=Set;f::internal::g_real_get_state=State;f::internal::g_real_init=Init;
  f::g_multi_frame_ready=[] {return true;};f::g_provider_mfg_ceiling=[] {return advertised_max;};
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
  f::g_multi_frame_ready=[] {return false;};
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);
  assert(received==3); // An optional addon backend never suppresses native Vulkan requests.
  f::g_force_multiplier=4;options.numFramesToGenerate=1;
  f::g_streamline_plugin_seen=false;f::g_streamline_max_generated=0;
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);
  assert(received==3&&pacing==0&&options.numFramesToGenerate==1); // No earlier GetState or scanner match required.
  f::internal::HookedSetOptions(sl::ViewportHandle(1),options);assert(received==3);
  f::internal::InvalidateLiveOptions();f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==3);
  sl::DLSSGState state{};state.structVersion=sl::kStructVersion4;
  f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==5&&state.numFramesActuallyPresented==2&&states==1);
  // An explicit menu trial does not wait for scanner or unrelated host gates.
  native_max=1;f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==5&&f::g_runtime_max_generated.load()==1);
  // Native maximum remains independently visible after scanner observations.
  f::g_multi_frame_ready=[] {return true;};f::g_streamline_max_generated=5;
  f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==5&&f::g_runtime_max_generated.load()==1);
  assert(state.numFramesActuallyPresented==2); // Never rewrite delivered-frame counts.
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==3);
  native_max=7;f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==7); // Larger native maximum is not reduced.
  advertised_max=17;f::g_streamline_max_generated=3;
  f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==17&&f::g_runtime_max_generated.load()==7);
  advertised_max=0;
  f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  assert(state.numFramesToGenerateMax==7); // Zero explicitly preserves native advertisement.
  result=sl::Result::eErrorUnsupportedInterface;f::internal::HookedGetState(sl::ViewportHandle(0),state,nullptr);
  result=sl::Result::eOk;f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==3);
  // Real API rejection triggers bounded retry and the unchanged game request.
  reject_multi=true;requests.clear();
  assert(f::internal::HookedSetOptions(sl::ViewportHandle(0),options)==sl::Result::eOk);
  assert((requests==std::vector<unsigned>{3,3,1}));assert(options.numFramesToGenerate==1&&pacing==0);
  assert(f::g_last_accepted_generated.load()==1);
  // An already rejected override is suppressed, but the real game call and its
  // failure still propagate. No repeated vendor rejection on every frame.
  result=sl::Result::eErrorUnsupportedInterface;requests.clear();
  assert(f::internal::HookedSetOptions(sl::ViewportHandle(0),options)==result);
  assert((requests==std::vector<unsigned>{1}));
  f::internal::InvalidateLiveOptions();requests.clear();
  assert(f::internal::HookedSetOptions(sl::ViewportHandle(0),options)==result);
  assert((requests==std::vector<unsigned>{3,3,1})); // A new lifecycle epoch permits a new trial.
  reject_multi=false;result=sl::Result::eOk;
  // D3D12 still attempts its pacing workaround, then forwards the actual request.
  o::SetResourceApi(o::ResourceApi::kD3D12);options.numFramesToGenerate=3;f::g_force_multiplier=0;
  f::internal::HookedSetOptions(sl::ViewportHandle(0),options);assert(received==3&&pacing>0);
  puts("vulkan_count_test PASS (native attempt/retry/fallback, real counts, no D3D pacer on Vulkan)");
}
