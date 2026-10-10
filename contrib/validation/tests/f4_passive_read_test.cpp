// SPDX-License-Identifier: MIT
#include <bit>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#ifndef F4_READ_HEADER
#define F4_READ_HEADER "../../../src/addons/mfgunlock/presentation_observer.hpp"
#endif
#include F4_READ_HEADER
namespace mfgunlock::presentation::passive {
#include "f4_passive_read_reference.hpp"
#include "f4_passive_read_candidate.hpp"
}
namespace p=mfgunlock::presentation::passive;
static size_t checks=0;
static void Check(bool good){++checks;if(!good)throw std::runtime_error("snapshot oracle mismatch");}
static void SameRate(const p::RateStats& a,const p::RateStats& b){
#define EQ(f) Check(a.f==b.f)
 EQ(intervals);EQ(count);EQ(span_ns);EQ(first_ns);EQ(last_ns);EQ(age_ns);EQ(valid);
 Check(std::bit_cast<uint64_t>(a.hz)==std::bit_cast<uint64_t>(b.hz));
#undef EQ
}
static void Same(const p::Snapshot& a,const p::Snapshot& b){
#define EQ(f) Check(a.f==b.f)
 EQ(bytes);EQ(version);EQ(enabled);EQ(capacity);EQ(generation);EQ(timestamp_ns);
 EQ(lost_events);EQ(invalid_events);EQ(native_viewport);EQ(native_viewport_conflict);
 EQ(displayed_frames_known);EQ(generated_frames_known);EQ(duplicates_known);EQ(dropped_known);
#undef EQ
 SameRate(a.source_callbacks,b.source_callbacks);SameRate(a.native_presented,b.native_presented);
 for(size_t i=0;i<p::kCpuKinds;++i){const auto& x=a.cpu[i];const auto& y=b.cpu[i];
#define EQ(f) Check(x.f==y.f)
  EQ(total);EQ(retained);EQ(warmup);EQ(overwritten);EQ(p50_ns);EQ(p95_ns);EQ(p99_ns);
  EQ(first_ns);EQ(last_ns);EQ(age_ns);EQ(valid);
#undef EQ
 }
}
int main(){try{
 std::mt19937_64 rng(0xF4001u);
 for(unsigned iteration=0;iteration<1500;++iteration){
  p::SetEnabled((iteration%3)!=0);
  const auto now=uint64_t{10000000000}+iteration;
  {std::lock_guard lock(p::g_lock);
   p::g_native_viewport=static_cast<uint32_t>(rng());p::g_native_conflict=(rng()%7)==0;
   p::g_lost=rng()%23;p::g_invalid=rng()%17;
   for(size_t k=0;k<p::kCpuKinds;++k){
    p::g_warmup[k]=static_cast<uint32_t>(rng()%9);
    for(uint64_t i=0,n=rng()%400;i<n;++i){
     const auto at=(i%7==0)?uint64_t{0}:(i%5==0?now+1:now-rng()%(p::kMaxAgeNs*2));
     p::g_cpu[k].Push({rng(),at,0});
    }
   }
   for(unsigned i=0,n=static_cast<unsigned>(rng()%400);i<n;++i){
    const auto at=now-rng()%(p::kMaxAgeNs*2);
    p::g_source.Push({1,at,rng()%1000000});p::g_native.Push({rng()%8,at,rng()%1000000});
   }
  }
  const auto reference=p::ReferenceRead(now);
  Same(reference,p::Read(now));Same(reference,p::CandidateRead(now));
  Same(p::ReferenceRead(now+p::kMaxAgeNs+1),p::CandidateRead(now+p::kMaxAgeNs+1));
 }
 // Candidate must still respect the real capture mutex, not read torn metadata.
 p::SetEnabled(false);
 auto run=[&](bool candidate){
  std::unique_lock owner(p::g_lock);
  auto future=std::async(std::launch::async,[&]{return candidate?p::CandidateRead(900):p::Read(900);});
  Check(future.wait_for(std::chrono::milliseconds(5))==std::future_status::timeout);
  p::g_invalid=81;owner.unlock();Check(future.get().invalid_events==81);
 };
 run(false);run(true);
 // A changing capture generation remains valid; no deadlock or state mutation by readers.
 std::thread writer([]{for(unsigned n=0;n<1500;++n){p::SetEnabled((n&1)!=0);p::SourceTick(n+1);}});
 for(unsigned n=0;n<1500;++n){const auto x=p::CandidateRead(n+1);Check(x.version==p::kVersion && x.capacity==p::kCapacity);}
 writer.join();p::SetEnabled(false);
 std::cout<<"f4_passive_read_test: "<<checks<<" checks PASS; oracle/production/C1; no GPU\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
