// SPDX-License-Identifier: MIT
// Actual addon.cpp callbacks compiled against explicit D3D12/ReShade doubles.
// This tests CPU ordering/ownership, not execution of HLSL or NVIDIA GPU work.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../../src/addons/mfgunlock/ui_alpha_backend.hpp"
#include "../../../src/addons/mfgunlock/ui_candidate.hpp"
#include "../../../src/addons/mfgunlock/output_state.hpp"

unsigned checks=0;
void Check(bool v,const char* s){++checks;if(!v)throw std::runtime_error(s);}
struct SRWLOCK { bool locked=false; };
#define SRWLOCK_INIT {}
void AcquireSRWLockExclusive(SRWLOCK* l){Check(!l->locked,"unexpected recursive alpha lock");l->locked=true;}
void ReleaseSRWLockExclusive(SRWLOCK* l){l->locked=false;}
uint32_t GetCurrentThreadId(){return 42;}
namespace sl {using CommandBuffer=void; enum class Result {eOk,eFailure};}
void InvalidateUiAlphaSource(uint64_t);
struct IUnknown { unsigned refs=1; virtual ~IUnknown()=default;
  void AddRef(){++refs;}
  void Release(){Check(refs>0,"source reference underflow");--refs;
    // Simulate synchronous ReShade destruction callback even while releasing
    // a helper-owned source; internal work must not re-enter the backend lock.
    InvalidateUiAlphaSource(reinterpret_cast<uint64_t>(this));
  }
};
namespace reshade::api {
enum class device_api {d3d12,vulkan};
enum class device_caps {compute_shader};
enum class format:uint32_t {r8_unorm=61,r8g8b8a8_unorm=28,r8g8b8a8_unorm_srgb=29};
enum class resource_type {texture_2d};
enum class resource_usage:uint32_t {general=0,unordered_access=8,shader_resource_non_pixel=64,shader_resource=192};
resource_usage operator|(resource_usage a,resource_usage b){return static_cast<resource_usage>(static_cast<uint32_t>(a)|static_cast<uint32_t>(b));}
enum class memory_heap {gpu_only};
enum class fence_flags {none};
enum class shader_stage {compute};
enum class pipeline_stage {compute_shader};
enum class descriptor_type {shader_resource_view,unordered_access_view};
enum class pipeline_subobject_type {compute_shader};
enum class command_queue_type:uint32_t {graphics=1,compute=2};
struct resource {uint64_t handle=0;};struct resource_view {uint64_t handle=0;};
struct pipeline_layout {uint64_t handle=0;};struct pipeline {uint64_t handle=0;};struct fence{uint64_t handle=0;};
struct texture_desc {uint64_t width=0;uint32_t height=0,depth_or_layers=1,levels=1,samples=1; api::format format=api::format::r8g8b8a8_unorm;};
struct resource_desc {resource_type type=resource_type::texture_2d;texture_desc texture{};
  resource_desc()=default;
  resource_desc(uint32_t w,uint32_t h,uint16_t d,uint16_t l,api::format f,uint16_t samples,memory_heap,resource_usage){texture={w,h,d,l,samples,f};}
};
struct resource_view_desc {api::format format; explicit resource_view_desc(api::format f):format(f){}};
struct descriptor_range {uint32_t binding=0,dx_register_index=0,dx_register_space=0,count=0,array_size=0;
 shader_stage visibility=shader_stage::compute; descriptor_type type=descriptor_type::shader_resource_view;};
struct pipeline_layout_param {explicit pipeline_layout_param(const descriptor_range&) {}};
struct shader_desc {const void* code=nullptr;size_t code_size=0;};
struct pipeline_subobject {pipeline_subobject_type type=pipeline_subobject_type::compute_shader;uint32_t count=0;const void* data=nullptr;};
struct descriptor_table_update {uint32_t binding=0,array_offset=0,count=0;descriptor_type type=descriptor_type::shader_resource_view;const void* descriptors=nullptr;};
struct rect{};struct swapchain{};
struct device {
  uint64_t next=100,completed=0; bool quiescent=true,fail_alloc=false,fail_view=false,fail_fence=false;
  unsigned destroys=0; std::map<uint64_t,resource_desc> descriptions;
  device_api get_api(){return device_api::d3d12;}
  bool check_capability(device_caps){return true;}
  bool check_format_support(api::format,resource_usage){return true;}
  bool create_fence(uint64_t,fence_flags,fence* f){if(fail_fence)return false;f->handle=++next;return true;}
  uint64_t get_completed_fence_value(fence){return completed;}
  void destroy_fence(fence){}
  bool create_pipeline_layout(uint32_t,const pipeline_layout_param*,pipeline_layout* l){l->handle=++next;return true;}
  void destroy_pipeline_layout(pipeline_layout){}
  bool create_pipeline(pipeline_layout,uint32_t,const pipeline_subobject*,pipeline* p){p->handle=++next;return true;}
  void destroy_pipeline(pipeline){}
  bool create_resource(const resource_desc& d,const void*,resource_usage,resource* r){if(fail_alloc)return false;r->handle=++next;descriptions[r->handle]=d;return true;}
  void destroy_resource(resource r){++destroys;descriptions.erase(r.handle);}
  resource_desc get_resource_desc(resource r){return descriptions.at(r.handle);}
  bool create_resource_view(resource,resource_usage,const resource_view_desc&,resource_view* v){if(fail_view)return false;v->handle=++next;return true;}
  void destroy_resource_view(resource_view){++destroys;}
  void set_resource_name(resource,const char*){}
};
struct command_list {
  std::vector<std::string> events;
  uint64_t get_native(){return reinterpret_cast<uint64_t>(this);}
  void barrier(resource,resource_usage,resource_usage){events.push_back("barrier");}
  void bind_pipeline(pipeline_stage,pipeline){events.push_back("pipeline");}
  void push_descriptors(shader_stage,pipeline_layout,uint32_t,const descriptor_table_update&){events.push_back("descriptor");}
  void dispatch(uint32_t,uint32_t,uint32_t){events.push_back("extract");}
};
struct command_queue {
  device* dev=nullptr;command_list commands;
  bool fail_signal=false,missing_list=false;uint64_t last_signal=0;
  unsigned flushes=0,waits=0;
  device* get_device(){return dev;}
  command_queue_type get_type(){return command_queue_type::graphics;}
  command_list* get_immediate_command_list(){return missing_list?nullptr:&commands;}
  void flush_immediate_command_list(){++flushes;commands.events.push_back("flush");dev->quiescent=false;}
  bool signal(fence,uint64_t value){commands.events.push_back("signal");if(fail_signal)return false;last_signal=value;return true;}
  void wait_idle(){++waits;dev->completed=last_signal;dev->quiescent=true;}
};
} // namespace reshade::api
std::array<unsigned char,4> __ui_alpha_extract{};
std::atomic<reshade::api::command_queue*> g_primary_present_queue{nullptr};
std::atomic<reshade::api::device*> g_primary_device{nullptr};
std::atomic<reshade::api::swapchain*> g_primary_swapchain{nullptr};
#include "ui_alpha_production_seam.hpp"
bool source_current=true;unsigned injected=0;
bool AcquireUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot* c){
  if (!source_current) return false;
  reinterpret_cast<IUnknown*>(c->native_resource)->AddRef();
  return true;
}
void ReleaseUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot* c,bool,sl::Result){reinterpret_cast<IUnknown*>(c->native_resource)->Release();}
void MarkUiCandidateInjected(const mfgunlock::framecount::UiCandidateSnapshot*){++injected;}
namespace api=reshade::api;
using Candidate=mfgunlock::framecount::UiCandidateSnapshot;
using Alpha=mfgunlock::framecount::UiAlphaCandidateSnapshot;
Candidate Make(IUnknown& resource,api::device& device,api::command_queue& queue,uint32_t w=1920){
 Candidate c{};c.output_revision=mfgunlock::outputstate::Read().revision;c.native_resource=reinterpret_cast<uint64_t>(&resource);c.width=w;c.height=1080;c.format=28;
 c.state=4;c.state_known=true;c.source_submitted=true;c.submission_queue=reinterpret_cast<uint64_t>(&queue);c.submission_thread=42;
 api::resource_desc d(w,1080,1,1,api::format::r8g8b8a8_unorm,1,api::memory_heap::gpu_only,api::resource_usage::shader_resource);
 device.descriptions[c.native_resource]=d;return c;
}
bool Prepare(const Candidate& c,Alpha& a){return PrepareUiAlphaCandidate(&c,reinterpret_cast<sl::CommandBuffer*>(1),&a);}
bool Finish(const Candidate& c,const Alpha& a,bool accepted=true){
 // The API double explicitly models SL recording its copy on the passed list.
 Check(a.copy_commands!=nullptr,"missing native command list");
 reinterpret_cast<api::command_list*>(a.copy_commands)->events.push_back("sl_clone_copy");
 return FinishUiAlphaCandidate(&c,&a,accepted,accepted?sl::Result::eOk:sl::Result::eFailure);
}
int main(){try{
 mfgunlock::outputstate::Observe(mfgunlock::outputstate::FormatKind::kRgb10A2Unorm,
     24,true,mfgunlock::outputstate::kColorSpaceHdr10Pq,true);
 api::device d;api::command_queue q;q.dev=&d;api::swapchain sw;
 g_primary_device=&d;g_primary_present_queue=&q;g_primary_swapchain=&sw;
 std::array<IUnknown,8> sources;std::array<Candidate,8> candidates;
 for(size_t i=0;i<sources.size();++i)candidates[i]=Make(sources[i],d,q);
 std::array<Alpha,4> pending;
 for(size_t i=0;i<4;++i){Check(Prepare(candidates[i],pending[i]),"free slot prepares");Check(sources[i].refs==2,"source retained before GPU work");Check(Finish(candidates[i],pending[i]),"optional copy accepted");}
 Alpha denied;
 Check(!Prepare(candidates[4],denied),"pool is bounded, never overwrite in-flight alpha");
 OnFinishPresentUiAlpha(&q,&sw);
 Check(!Prepare(candidates[4],denied),"CPU Present doesn't release pending slots");
 d.completed=UINT64_MAX;OnFinishPresentUiAlpha(&q,&sw);
 Check(!Prepare(candidates[4],denied),"removed device sentinel never authorizes reuse");
 d.completed=1;OnFinishPresentUiAlpha(&q,&sw);
 Check(Prepare(candidates[4],denied),"one completed copy gives exactly one slot");
 Check(sources[0].refs==1 && sources[1].refs==2,"only completed slot releases its source");
 Check(Finish(candidates[4],denied),"next slot completed");
 Check(q.waits==0,"zero per-frame CPU waits while source resources change");
 // Verify each copy is recorded between extraction and submission.
 size_t extracts=0,copies=0,flushes=0;
 for(const auto& event:q.commands.events){if(event=="extract")++extracts;if(event=="sl_clone_copy"){Check(extracts>copies,"copy must follow extraction");++copies;}if(event=="flush"){Check(copies>flushes,"flush must follow copy");++flushes;}}
 Check(copies==5 && flushes==5,"exactly one queued extraction/copy transaction per accepted helper");
 d.completed=q.last_signal;OnFinishPresentUiAlpha(&q,&sw);
 Alpha invalidated;Check(Prepare(candidates[5],invalidated),"fresh alpha");
 InvalidateUiAlphaPending();
 Check(!Finish(candidates[5],invalidated),"resize cannot publish stale Ready");
 Check(g_ui_alpha_backend.slots[invalidated.slot].state.gpu_pending,"invalidated GPU lease kept until fence");
 d.completed=q.last_signal;OnFinishPresentUiAlpha(&q,&sw);
 Alpha old_output;Check(Prepare(candidates[6],old_output),"output change test prepares");
 mfgunlock::outputstate::Reset();
 Check(!Finish(candidates[6],old_output),"output generation changed: no stale Ready despite accepted tag");
 Check(g_ui_alpha_backend.slots[old_output.slot].state.gpu_pending,"changed output still protects queued copy");
 for(auto& c:candidates)c.output_revision=mfgunlock::outputstate::Read().revision;
 d.completed=q.last_signal;OnFinishPresentUiAlpha(&q,&sw);
 Alpha failed;Check(Prepare(candidates[6],failed),"rejected tag test prepares");
 Check(!Finish(candidates[6],failed,false),"native reject is not Ready");
 Check(g_ui_alpha_backend.slots[failed.slot].state.gpu_pending,"failed tag can have queued work");
 d.completed=q.last_signal;OnFinishPresentUiAlpha(&q,&sw);
 Check(Prepare(candidates[7],failed),"signal failure test prepares");q.fail_signal=true;
 Check(!Finish(candidates[7],failed),"failed signal rejects readiness");q.fail_signal=false;
 Check(g_ui_alpha_backend.slots[failed.slot].state.poisoned,"failed signal retains source and output");
 const auto old_ticket=failed.ticket;
 DestroyUiAlphaBackend(&d,true);
 Check(q.waits==1,"teardown is the only CPU wait");
 for(const auto& source:sources)Check(source.refs==1,"all retained references retired at teardown");
 Check(Prepare(candidates[0],denied),"device/queue reused after teardown");
 Check(denied.ticket!=old_ticket,"lease tickets never reuse after backend reset");
 Check(!FinishUiAlphaCandidate(&candidates[7],&failed,true,sl::Result::eOk),"late completion cannot mutate new slot");
 Check(Finish(candidates[0],denied),"new lease unaffected by stale callback");
 d.completed=q.last_signal;OnFinishPresentUiAlpha(&q,&sw);
 source_current=false;Check(!Prepare(candidates[0],denied),"cached source SRV still revalidates candidate generation");source_current=true;
 q.missing_list=true;Check(!Prepare(candidates[1],denied),"missing immediate list is not Ready");q.missing_list=false;
 api::command_queue foreign;foreign.dev=&d;auto wrong=Make(sources[1],d,foreign);
 Check(!Prepare(wrong,denied),"foreign queue cannot submit to primary fence timeline");
 auto huge=Make(sources[1],d,q,16384);huge.height=16384;d.descriptions[huge.native_resource].texture.height=16384;
 Check(!Prepare(huge,denied),"bounded output allocation budget");
 DestroyUiAlphaBackend(&d,true);
 std::cout<<"ui_alpha_gpu_test: "<<checks<<" checks PASS (API doubles, GPU NOT_RUN)\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
