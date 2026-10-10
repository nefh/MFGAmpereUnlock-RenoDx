// SPDX-License-Identifier: MIT
// Uses production SL entry hooks. No helper renderer or provider code executed.
#include "framecount.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace fc=mfgunlock::framecount;
namespace os=mfgunlock::outputstate;
struct Tag {uint32_t type;const void* native;};
struct Batch {uint32_t frame;sl::CommandBuffer* commands;std::vector<Tag> tags;};
std::vector<Batch> forwarded;
unsigned checks=0, observations=0;
void Check(bool value,const char* name){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
bool Observe(fc::UiCandidateSnapshot* c){++observations;*c={};return false;}
sl::Result NativeFrame(const sl::FrameToken& frame,const sl::ViewportHandle&,
 const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* commands){
 Batch b{static_cast<uint32_t>(frame),commands,{}};
 for(uint32_t i=0;i<count;++i)b.tags.push_back({tags[i].type,tags[i].resource?tags[i].resource->native:nullptr});
 forwarded.push_back(b);return sl::Result::eOk;
}
sl::Result Native(const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* commands){return NativeFrame(0,viewport,tags,count,commands);}
void Reset(){
 forwarded.clear();observations=0;mfgunlock::g_enabled=true;fc::g_ui_composition_enabled=true;
 fc::internal::ForgetUiOutputs();
 for(auto& state:fc::internal::g_ui_viewports){fc::internal::ClearAutomaticUiTagState(&state);state.options_seen=false;}
 fc::internal::g_real_set_tag=Native;fc::internal::g_real_set_tag_for_frame=NativeFrame;
 fc::g_observe_ui_candidate=Observe;fc::g_prepare_ui_alpha_candidate=nullptr;
 fc::g_finish_ui_alpha_candidate=nullptr;
 fc::g_ui_candidate_injection_enabled=true;fc::g_ui_candidate_options_synced=true;
 os::Observe(os::FormatKind::kRgb10A2Unorm,24,true,os::kColorSpaceHdr10Pq,true);
 auto* state=fc::internal::GetUiState(sl::ViewportHandle(1));
 state->options_seen=true;state->mode=static_cast<uint32_t>(sl::DLSSGMode::eOn);
 state->color_width=1920;state->color_height=1080;state->color_format=24;
}
sl::Resource Resource(uint64_t id,uint32_t fmt){sl::Resource r(sl::ResourceType::eTex2d,reinterpret_cast<void*>(id),4);r.width=1920;r.height=1080;r.nativeFormat=fmt;return r;}
int main(int argc,char** argv){
 const std::string select=argc>1?argv[1]:"all";
 auto run=[&](const char* name){return select=="all"||select==name;};
 auto hud=Resource(0x1000,24),ui=Resource(0x2000,28),alpha=Resource(0x3000,61);
 sl::ResourceTag h(&hud,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent);
 sl::ResourceTag u(&ui,sl::kBufferTypeUIColorAndAlpha,sl::ResourceLifecycle::eValidUntilPresent);
 sl::ResourceTag a(&alpha,sl::kBufferTypeUIAlpha,sl::ResourceLifecycle::eValidUntilPresent);
 auto* commands=reinterpret_cast<sl::CommandBuffer*>(0x99);sl::ViewportHandle viewport(1);
 if(run("permutations")){
  std::array<sl::ResourceTag,3> tags{h,u,a};std::array<int,3> order{0,1,2};
  do{Reset();auto batch=tags;for(size_t i=0;i<3;++i)batch[i]=tags[static_cast<size_t>(order[i])];
   Check(fc::internal::HookedSetTagForFrame(1,viewport,batch.data(),3,commands)==sl::Result::eOk,"native batch result");
   Check(forwarded.size()==1 && forwarded[0].tags.size()==3 && observations==0,"native entire batch wins in every order");
   for(size_t i=0;i<3;++i)Check(forwarded[0].tags[i].native==batch[i].resource->native,"native tag preserved");
  }while(std::next_permutation(order.begin(),order.end()));
 }
 if(run("split_ui_first")){
  Reset();fc::internal::HookedSetTagForFrame(7,viewport,&u,1,commands);
  Check(forwarded.size()==1 && forwarded[0].tags[0].native==ui.native,"first native UI must not be nulled before HUDless arrives");
  fc::internal::HookedSetTagForFrame(7,viewport,&h,1,commands);
  Check(observations==0 && forwarded.size()==2,"later HUDless must not replace earlier native UI");
 }
 if(run("split_hud_first")){
  Reset();fc::internal::HookedSetTagForFrame(7,viewport,&h,1,commands);
  fc::internal::HookedSetTagForFrame(7,viewport,&u,1,commands);
  Check(forwarded.size()==2 && forwarded[1].tags[0].native==ui.native,"split native UI after HUDless is preserved immediately");
 }
 if(run("explicit_null")){
  Reset();sl::ResourceTag clear(nullptr,sl::kBufferTypeUIAlpha,sl::ResourceLifecycle::eValidUntilPresent);
  fc::internal::HookedSetTagForFrame(8,viewport,&clear,1,commands);
  fc::internal::HookedSetTagForFrame(8,viewport,&h,1,commands);
  Check(observations==0,"explicit native UI clear is intent, not permission to inject guessed UI");
  fc::internal::HookedSetTagForFrame(9,viewport,&h,1,commands);
  Check(observations==1,"new source frame expires prior native intent rather than permanently disabling helper");
 }
 if(run("replace_helper")){
  Reset();auto* state=fc::internal::GetUiState(viewport);
  fc::internal::MarkAutomaticUiTag(state,sl::kBufferTypeUIAlpha,false);
  std::array<sl::ResourceTag,2> tags{h,u};
  fc::internal::HookedSetTag(viewport,tags.data(),2,commands);
  Check(forwarded.size()==1 && forwarded[0].tags.size()==3,"native ColorAlpha cancels only our old alpha tag");
  Check(forwarded[0].tags[0].native==hud.native && forwarded[0].tags[1].native==ui.native,"native resources preserved");
  Check(forwarded[0].tags[2].type==sl::kBufferTypeUIAlpha && forwarded[0].tags[2].native==nullptr,"old preferred helper alpha cannot shadow native color");
  Check(!state->automatic_ui_tag_global,"helper no longer owns native tag");
 }
 if(run("replace_frame_helper")){
  Reset();fc::internal::HookedSetTagForFrame(13,viewport,&h,1,commands);
  auto* state=fc::internal::GetUiState(viewport);
  fc::internal::MarkAutomaticUiTag(state,sl::kBufferTypeUIAlpha,true);
  forwarded.clear();
  fc::internal::HookedSetTagForFrame(13,viewport,&u,1,commands);
  Check(forwarded.size()==1 && forwarded[0].tags.size()==2,"native UI in later frame batch retires earlier helper alpha");
  Check(forwarded[0].tags[0].native==ui.native && forwarded[0].tags[1].type==sl::kBufferTypeUIAlpha &&
        forwarded[0].tags[1].native==nullptr,"only helper alpha cleared in matching source frame");
 }
 if(run("frame_generation")){
  Reset();std::array<sl::ResourceTag,2> tags{h,u};
  fc::internal::HookedSetTagForFrame(10,viewport,tags.data(),2,commands);
  Check(fc::internal::GetUiState(viewport)->ui_recomposition_eligible,"complete current pair eligible");
  fc::internal::HookedSetTagForFrame(11,viewport,&h,1,commands);
  Check(!fc::internal::GetUiState(viewport)->ui_recomposition_eligible,"old source frame UI cannot make new incomplete pair eligible");
 }
 if(run("unknown_abi")){
  Reset();auto unknown=u;unknown.structVersion=99;
  std::array<sl::ResourceTag,2> tags{h,unknown};
  fc::internal::HookedSetTag(viewport,tags.data(),2,commands);
  Check(forwarded.size()==1 && forwarded[0].tags[1].native==ui.native,"unknown native ABI forwarded rather than reinterpreted or nulled");
  Check(observations==0,"unknown native contract doesn't authorize helper");
 }
 if(run("volatile_replay")){
  Reset();os::Observe(os::FormatKind::kRgba8Unorm,28,true,os::kColorSpaceSrgbNonlinear,true);
  fc::g_acquire_ui_candidate=[](const fc::UiCandidateSnapshot*){return true;};
  fc::g_release_ui_candidate=[](const fc::UiCandidateSnapshot*,bool,sl::Result){};
  fc::UiCandidateSnapshot c{};c.native_resource=0x100;c.width=1920;c.height=1080;c.format=29;
  c.reject_reasons=mfgunlock::uicandidate::kAccept;c.recent=true;c.state_known=true;c.state=0x40;
  c.stable_frames=mfgunlock::uicandidate::kRequiredStableFrames;c.ui_encoding=static_cast<uint32_t>(os::UiEncoding::kSdrSrgb);
  unsigned native_count=0,optional_count=0;
  auto result=fc::internal::TryInjectUiCandidate(nullptr,c,&h,1,commands,false,
    [&](const sl::ResourceTag* tags,uint32_t n){bool reject=false;for(uint32_t i=0;i<n;++i){
      if(tags[i].type==sl::kBufferTypeHUDLessColor && tags[i].resource)++native_count;
      if(tags[i].type==sl::kBufferTypeUIColorAndAlpha && tags[i].resource){++optional_count;reject=true;}}
      return reject?sl::Result::eErrorUnsupportedInterface:sl::Result::eOk;});
  Check(native_count==1 && optional_count==1 && result==sl::Result::eOk,"optional SDR failure never repeats a native volatile batch");
 }
 if(run("viewport_isolation")){
  Reset();fc::internal::HookedSetTagForFrame(2,viewport,&u,1,commands);
  sl::ViewportHandle other(2);auto* state=fc::internal::GetUiState(other);
  state->options_seen=true;state->mode=static_cast<uint32_t>(sl::DLSSGMode::eOn);
  state->color_width=1920;state->color_height=1080;state->color_format=24;
  fc::internal::HookedSetTagForFrame(2,other,&h,1,commands);
  Check(!state->ui_recomposition_eligible,"UI from another viewport cannot complete a pair");
 }
 std::printf("ui_native_precedence_test: %u checks PASS\n",checks);
}
