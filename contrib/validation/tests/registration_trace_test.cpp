// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include "../../../src/addons/mfgunlock/registration_trace.hpp"
#include "../../../src/addons/mfgunlock/addon_entry.hpp"
using mfgunlock::registrationtrace::Journal;
int main() {
  Journal<4,32> j;
  unsigned n=0; j.Visit([&](auto,const char*){ ++n; }); assert(n==0);
  j.Append("early=%d",1); j.Append("later=%d",2);
  j.Visit([&](auto i,const char* s){ assert(std::strcmp(s,i==0?"early=1":"later=2")==0); ++n; });
  assert(n==2 && j.Dropped()==0);
  const std::string long_text(64, 'x');
  j.Append("%s",long_text.c_str()); j.Append("last"); j.Append("overflow");
  n=0; j.Visit([&](auto i,const char* s){ if(i==2) assert(std::strlen(s)==31); ++n; });
  assert(n==4 && j.Dropped()==1);
  // Multiple writers publish complete immutable records, no races on payload.
  Journal<1024,48> parallel;
  std::vector<std::thread> threads;
  for(int t=0;t<4;++t) threads.emplace_back([&parallel,t](){for(int i=0;i<200;++i)parallel.Append("writer=%d item=%d",t,i);});
  for(auto& thread:threads)thread.join();
  n=0; parallel.Visit([&](auto,const char* s){assert(std::strncmp(s,"writer=",7)==0);++n;});
  assert(n==800 && parallel.Dropped()==0);
  // Reproduce the logical gap, WITHOUT calling it a fix: a local READY flag
  // cannot prove that the host still has its registration record.
  mfgunlock::addonentry::State state;
  int self,host; bool host_registered=false; unsigned attempts=0;
  auto reg=[&](void*,void*){host_registered=true; ++attempts; return true;};
  auto setup=[](){};
  assert(state.Attach(&self,&host,[]()->void*{return nullptr;},reg,setup)==mfgunlock::addonentry::Result::kReady);
  host_registered=false;
  assert(state.Attach(&self,&host,[]()->void*{return nullptr;},reg,setup)==mfgunlock::addonentry::Result::kAlreadyReady);
  assert(!host_registered && attempts==1);
  std::cout << "registration_trace_test PASS: bounded journal, truncation, concurrent writers, stale-READY reproduction (not a runtime fix)\n";
}
