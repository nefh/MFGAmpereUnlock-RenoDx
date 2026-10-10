// SPDX-License-Identifier: MIT
// Instrumentation-only CPU microbenchmark. NOT a game/GPU performance claim.
#include "../../../src/addons/mfgunlock/presentation_observer.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#ifndef __VERSION__
#define __VERSION__ "MSVC (record compiler command/version separately)"
#endif
namespace p = mfgunlock::presentation::passive;
int main() {
  constexpr uint32_t loops = 200000, repeats = 7;
  std::array<double,repeats> off{}, on{};
  uint64_t sink=0;
  for (uint32_t trial=0; trial<repeats; ++trial) {
    for (unsigned int step=0; step<2; ++step) {
      const bool capture=((step+trial)&1)!=0;
      p::SetEnabled(capture);
      const auto begin=p::NowNs();
      for(uint32_t i=0;i<loops;++i) {
        p::CpuScope own(p::CpuKind::kTag);
        sink+=p::CallNative([&] { return uint64_t{i & 7u}; });
      }
      const double each=double(p::NowNs()-begin)/loops;
      (capture?on:off)[trial]=each;
    }
  }
  p::SetEnabled(false);
  auto median=[](auto a){std::sort(a.begin(),a.end());return a[a.size()/2];};
  std::printf("{\n  \"scope\": \"CPU instrumentation microbenchmark, no GPU/game/provider execution\",\n");
  std::printf("  \"compiler\": \"%s\",\n", __VERSION__);
  std::printf("  \"iterations_per_trial\": %u, \"trials\": %u, \"sink\": %llu,\n", loops,repeats,static_cast<unsigned long long>(sink));
  std::printf("  \"off_median_ns_per_call\": %.3f, \"on_median_ns_per_call\": %.3f,\n",median(off),median(on));
  std::printf("  \"off_trials_ns\": [");for(uint32_t i=0;i<repeats;++i)std::printf("%s%.3f",i?", ":"",off[i]);std::printf("],\n");
  std::printf("  \"on_trials_ns\": [");for(uint32_t i=0;i<repeats;++i)std::printf("%s%.3f",i?", ":"",on[i]);std::printf("]\n}\n");
}
