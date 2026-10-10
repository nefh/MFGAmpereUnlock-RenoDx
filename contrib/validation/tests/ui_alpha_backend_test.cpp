// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>
#include "../../../src/addons/mfgunlock/ui_alpha_backend.hpp"
namespace b = mfgunlock::uialphabackend;
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v){std::cerr<<why<<'\n';std::exit(1);}}
uint64_t Record(b::State& s){
  Check(b::Prepare(s,1,2,1920,1080,28,4,true,true,7),"prepare after completed lifetime");
  const auto ticket=s.pending.generation;
  Check(b::ReadyForExtraction(s),"ready for recording");
  b::FinishExtraction(s,true);Check(b::ReadyToTag(s),"ready only after extraction recorded");
  return ticket;
}
int main(){
  Check(!b::CanPrepare(0,2,1920,1080,28,true,true,7,7),"no resource");
  Check(!b::CanPrepare(1,0,1920,1080,28,true,true,7,7),"no queue");
  Check(!b::CanPrepare(1,2,0,1080,28,true,true,7,7),"no extent");
  Check(!b::CanPrepare(1,2,1920,1080,28,false,true,7,7),"state unknown");
  Check(!b::CanPrepare(1,2,1920,1080,28,true,false,7,7),"source unsubmitted");
  Check(!b::CanPrepare(1,2,1920,1080,28,true,true,7,8),"foreign submission thread");
  b::State s{};
  Check(b::Prepare(s,1,2,10,10,28,4,true,true,7),"prepare");
  b::FinishExtraction(s,false);
  Check(!b::ReadyToTag(s) && s.extraction_failures==1 && !s.gpu_pending,"no GPU work on pre-dispatch failure");
  auto ticket=Record(s);
  Check(!b::Reclaim(s,UINT64_MAX),"prepared lease cannot be reclaimed");
  Check(b::SealCopy(s,ticket,true,true,true,9),"copy accepted and signal recorded");
  Check(!b::SealCopy(s,ticket,true,true,true,9),"duplicate completion rejected");
  Check(!b::Prepare(s,1,2,10,10,28,4,true,true,7),"CPU return is not permission to overwrite");
  Check(!b::Reclaim(s,8),"delayed GPU copy");
  Check(!b::Reclaim(s,UINT64_MAX),"device lost sentinel is not successful completion");
  Check(b::Reclaim(s,9),"only completing the copy fence releases source and staging");
  ticket=Record(s); b::Invalidate(s);
  Check(!b::ReadyToTag(s) && s.pending.extraction_recorded,"resize invalidates readiness not GPU lifetime");
  Check(!b::SealCopy(s,ticket,true,true,true,10) && s.gpu_pending,"stale completion still seals lifetime but not Ready");
  Check(!b::Reclaim(s,9) && b::Reclaim(s,10),"stale work must finish before reuse");
  ticket=Record(s);
  Check(!b::SealCopy(s,ticket,true,false,true,11) && s.gpu_pending,"failed tag can have recorded copy work");
  Check(!b::Reclaim(s,10) && b::Reclaim(s,11),"failed optional tag obeys copy lifetime");
  ticket=Record(s);
  Check(!b::SealCopy(s,ticket,true,true,false,12) && s.poisoned,"failed signal quarantines source and output");
  b::Invalidate(s);
  Check(!b::Reclaim(s,100) && !b::Prepare(s,1,2,10,10,28,4,true,true,7),"invalidation cannot clear poison");
  for (uint64_t signal : {uint64_t{0},UINT64_MAX}) {
    b::State t{};const auto id=Record(t);
    Check(!b::SealCopy(t,id,true,true,true,signal) && t.poisoned,"invalid fence timeline quarantined");
  }
  b::State lost{}; ticket=Record(lost);
  Check(!b::SealCopy(lost,ticket,false,false,false,0) && lost.poisoned,"queue loss retains resources");
  for(unsigned i=0;i<128;++i){b::State t{};auto id=Record(t);Check(b::SealCopy(t,id,true,true,true,i+1),"cycle seal");Check(b::Reclaim(t,i+1),"cycle reclaim");}
  std::cout<<"ui_alpha_backend_test: "<<checks<<" checks PASS\n";
}
