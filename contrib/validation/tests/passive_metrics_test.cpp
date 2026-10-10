// SPDX-License-Identifier: MIT
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <future>
#include "../../../src/addons/mfgunlock/presentation_observer.hpp"
namespace p=mfgunlock::presentation::passive;
unsigned checks=0;
void Check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
int main(){try{
 static_assert(std::is_standard_layout_v<p::Snapshot> && std::is_trivially_copyable_v<p::Snapshot>);
 p::SetEnabled(false);p::SourceTick(100);Check(!p::Read(100).source_callbacks.valid,"disabled capture has no fabricated cadence");
 p::SetEnabled(true);const auto generation=p::g_generation.load();
 for(uint32_t i=0;i<8;++i)p::RecordCpu(p::CpuKind::kTag,generation,100+i,1000+i);
 Check(p::Read(1010).cpu[0].retained==0,"warmup excluded from percentile window");
 for(uint32_t i=1;i<=100;++i)p::RecordCpu(p::CpuKind::kTag,generation,i,2000+i);
 auto value=p::Read(2200);
 Check(value.cpu[0].p50_ns==50 && value.cpu[0].p95_ns==95 && value.cpu[0].p99_ns==99,"exact nearest-rank quantiles");
 Check(value.cpu[0].warmup==8 && value.cpu[0].retained==100,"explicit sample and warmup counts");
 p::Reset();p::RecordCpu(p::CpuKind::kTag,generation,999,3000);
 Check(p::Read(3000).invalid_events==1 && p::Read(3000).cpu[0].retained==0,"late scope from old device generation rejected");
 auto epoch=p::g_generation.load();
 for(uint32_t i=0;i<600;++i)p::RecordCpu(p::CpuKind::kGetState,epoch,i,10000+i);
 value=p::Read(11000);
 Check(value.cpu[2].retained==p::kCapacity && value.cpu[2].total==592 && value.cpu[2].overwritten==336,"bounded storage with explicit evictions");
 Check(!p::Read(p::kMaxAgeNs+11001).cpu[2].valid,"stale CPU percentiles unknown");
 p::Reset();
 const uint64_t start=1000000000, period=16683333; // 60000/1001 Hz, not rounded to 60
 p::SourceTick(start);
 for(uint32_t i=1;i<=20;++i)p::SourceTick(start+period*i);
 value=p::Read(start+period*20);
 Check(value.source_callbacks.valid && std::abs(value.source_callbacks.hz-59.94006)<0.0001,"fractional refresh retained");
 p::SourceTick(start+period*20);
 Check(!p::Read(start+period*20).source_callbacks.valid,"zero interval resets source window");
 p::Reset();p::NativeReport(start,7,1000,true);
 p::NativeReport(start+10000000,7,3,true);p::NativeReport(start+20000000,7,2,true);
 value=p::Read(start+20000000);
 Check(value.native_presented.valid && value.native_presented.count==5 && std::abs(value.native_presented.hz-250)<0.001,"native deltas once, first counter has unknown interval");
 Check(!value.displayed_frames_known && !value.generated_frames_known && !value.duplicates_known && !value.dropped_known,"reported counts never imply physical display/generation");
 p::NativeReport(start+30000000,8,5,true);
 Check(!p::Read(start+30000000).native_presented.valid && p::Read(start+30000000).native_viewport_conflict,"don't mix native counters from different viewports");
 p::Reset();p::NativeReport(start,7,3,true);p::NativeReport(start+1000,7,3,false);
 p::NativeReport(start+2000,7,10,true);
 Check(!p::Read(start+2000).native_presented.valid,"failed call requires a fresh interval baseline");
 p::NativeReport(start+2000,7,3,true);
 Check(!p::Read(start+2000).native_presented.valid,"invalid timestamp cannot report rate");
 p::ThreadSample sample{};sample.Pause(10);sample.Pause(20);sample.Resume(25);sample.Resume(40);
 Check(sample.excluded==30 && sample.pauses==0,"nested native calls excluded once");
 p::SetEnabled(true);
 for(unsigned n=0;n<20;++n){p::CpuScope own(p::CpuKind::kSetOptions);p::CallNative([]{p::CpuScope inner(p::CpuKind::kTag);return p::CallNative([]{return 7;});});}
 value=p::Read();
 Check(p::g_thread_depth==0 && value.cpu[1].total==12 && value.cpu[0].total==12,"actual scoped exclusion handles recursive native callbacks");
 p::Reset();const auto drops=p::g_lost.load();
 std::promise<void> locked, release;
 auto released=release.get_future();
 std::thread holder([&]{std::lock_guard guard(p::g_lock);locked.set_value();released.wait();});
 locked.get_future().wait();
 p::RecordCpu(p::CpuKind::kTag,p::g_generation.load(),1,10);
 release.set_value();holder.join();
 Check(p::g_lost.load()==drops+1,"hot path drops instead of blocking on snapshot lock");
 p::Reset();p::NativeReport(start,7,5,true);p::NativeReport(start+1000,7,5,true);
 auto stale_epoch=p::g_generation.load();p::Reset();
 p::NativeReport(start+2000,7,7,true,stale_epoch);
 Check(!p::Read(start+2000).native_presented.valid && p::Read(start+2000).invalid_events==1,"late native call cannot seed a new capture generation");
 p::NativeReport(start+3000,7,UINT32_MAX,true);
 Check(!p::Read(start+3000).native_presented.valid,"saturated native counter is not a measured rate");
 p::Reset();p::NativeReport(start,7,5,true);p::NativeReport(start+1000,7,5,true);
 p::g_native_gap.store(true); // producer's nonblocking drop marker
 p::NativeReport(start+3000,7,10,true);
 Check(!p::Read(start+3000).native_presented.valid,"lost native sample invalidates consumed-counter baseline");
 p::NativeReport(start+4000,7,2,true);p::NativeReport(start+5000,7,3,true);
 Check(p::Read(start+5000).native_presented.count==5 && p::Read(start+5000).native_presented.span_ns==2000,"only complete post-gap intervals enter cadence");
 p::SourceTick(start);p::SourceTick(start+1000);p::g_source_gap.store(true);
 p::SourceTick(start+2000);
 Check(!p::Read(start+2000).source_callbacks.valid,"lost source callback invalidates callback rate instead of inventing a longer frame");
 p::SetEnabled(false);Check(!p::Read().enabled,"capture off closes session");
 std::cout<<"passive_metrics_test: "<<checks<<" checks PASS\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
