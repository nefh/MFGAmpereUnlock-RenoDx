// SPDX-License-Identifier: MIT
#ifndef F4_READ_HEADER
#error supply F4_READ_HEADER
#endif
#include F4_READ_HEADER
#include <cstdio>
#include <algorithm>
#include <array>
namespace p = mfgunlock::presentation::passive;
int main(){
 constexpr unsigned trials=11, loops=20000;
 std::array<std::array<double,trials>,3> values{};
 uint64_t sink=0;
 for(unsigned t=0;t<trials;++t){
  for(unsigned round=0;round<3;++round){
   const unsigned mode=(t+round)%3;
   p::SetEnabled(mode!=0);
   const uint64_t epoch=p::g_generation.load(), start=1000000000;
   if(mode==2){for(unsigned i=0;i<264;++i){
    for(unsigned k=0;k<p::kCpuKinds;++k)p::RecordCpu(static_cast<p::CpuKind>(k),epoch,i%83,start+i*1000);
    p::SourceTick(start+i*1000);p::NativeReport(start+i*1000,1,3,true);
   }}
   const auto begin=p::NowNs();
   for(unsigned i=0;i<loops;++i){
    const auto snap=p::Read(start+300000+i);
    sink+=snap.generation+snap.cpu[0].retained+snap.cpu[3].p99_ns+snap.native_presented.count+snap.timestamp_ns;
   }
   values[mode][t]=double(p::NowNs()-begin)/loops;
  }
 }
 p::SetEnabled(false);
 const char* names[]={"capture_off","capture_on_empty","capture_on_full"};
 std::printf("{\"scope\":\"CPU Read snapshot microbenchmark only, no GPU\",\"loops\":%u,\"trials\":%u,\"sink\":%llu,\"timings\":{",loops,trials,(unsigned long long)sink);
 for(unsigned m=0;m<3;++m){auto sorted=values[m];std::sort(sorted.begin(),sorted.end());
  std::printf("%s\"%s\":{\"median_ns\":%.3f,\"trials_ns\":[",m?",":"",names[m],sorted[trials/2]);
  for(unsigned t=0;t<trials;++t){std::printf("%s%.3f",t?",":"",values[m][t]);}
  std::printf("]}");
 }
 std::printf("}}\n");
}
