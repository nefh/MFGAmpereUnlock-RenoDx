// SPDX-License-Identifier: MIT
#include "mfg_gate_fixture.hpp"
#include <iostream>
#include <fstream>
using namespace mfg_gate_fixture;
namespace p=mfgunlock::provider;
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
  if(argc==3 && std::string(argv[1])=="--fixture") {
    Image fixture;std::ofstream out(argv[2],std::ios::binary);
    out.write(reinterpret_cast<const char*>(fixture.bytes.data()),static_cast<std::streamsize>(fixture.bytes.size()));
    return out.good()?0:2;
  }
  Reset();Image image;p::MfgGatePlan plan;
  auto first=p::FindMfgGatePlan(image.Module(),plan);if(!first)std::cerr<<plan.contract.reason<<"\n";
  Check(first,"unknown image structurally qualifies");
  Check(plan.contract.ceiling==3,"ceiling is dataflow-derived, not reference five");
  Check(plan.contract.sites[0].immediate==image.cap_site&&plan.contract.sites[1].immediate==image.run_site,"expected addresses are output only");
  const auto original=image.bytes;
  for(auto arch:{mfgunlock::Architecture::kAmpere,mfgunlock::Architecture::kTuring,mfgunlock::Architecture::kAda}){
    Reset();image.Bind();mfgunlock::architecture::Configure(arch);p::MfgGatePlan current;
    Check(p::FindMfgGatePlan(image.Module(),current),"all architectures discover same gate contract");
    std::vector<p::internal::BytePatch> undo;
    Check(p::ApplyMfgGatePlan(current,undo)&&undo.size()==2,"paired transaction committed");
    Check(image.bytes[image.cap_site]==static_cast<unsigned char>(mfgunlock::architecture::ActiveProfile()->native_arch),"native architecture threshold written");
    Check(!p::ApplyMfgGatePlan(current,undo),"stale/prewrite plan not applied twice");
    Check(p::RestoreMfgComparisons(undo)&&undo.empty()&&image.bytes==original,"byte-identical rollback");
  }
  for(size_t failure:{1u,2u,3u,4u}){
    Reset();image.Bind();Check(p::FindMfgGatePlan(image.Module(),plan),"failure plan ready");provider_mock::fail_protection_call=failure;
    std::vector<p::internal::BytePatch> undo;Check(!p::ApplyMfgGatePlan(plan,undo),"injected protection failure denies commit");
    Check(undo.empty()&&image.bytes==original,"failed transaction restores originals");
  }
  Reset();image.Bind();p::FindMfgGatePlan(image.Module(),plan);provider_mock::fail_protection_calls={3,4};std::vector<p::internal::BytePatch> undo;
  Check(!p::ApplyMfgGatePlan(plan,undo)&&!undo.empty(),"failed restore retains undo owner");provider_mock::fail_protection_calls.clear();
  Check(p::RestoreMfgComparisons(undo)&&image.bytes==original,"cleanup retry recovers original bytes");
  for(unsigned fail:{1u,2u}){
    Reset();image.Bind();p::FindMfgGatePlan(image.Module(),plan);provider_mock::fail_flush_call=fail;
    std::vector<p::internal::BytePatch> changes;
    Check(!p::ApplyMfgGatePlan(plan,changes)&&changes.empty()&&image.bytes==original,"FlushInstructionCache failure rolls back entire plan");
  }
  Reset();image.Bind();p::FindMfgGatePlan(image.Module(),plan);provider_mock::corrupt_flush_call=1;
  std::vector<p::internal::BytePatch> damaged;
  Check(!p::ApplyMfgGatePlan(plan,damaged)&&!damaged.empty(),"readback fault retains recovery metadata");
  for(const auto& d:damaged)*d.address=d.after;
  provider_mock::corrupt_flush_call=0;
  Check(p::RestoreMfgComparisons(damaged)&&image.bytes==original,"retained recovery completes after interference cleared");
  for(size_t offset:{size_t{0},size_t{0x88},size_t{0x6000},size_t{0x5100},size_t{0x5200},size_t{0x5400},size_t{0x4500},size_t{image.run_site},size_t{image.flag_load+3}}){
    Reset();image.Bind();Check(p::FindMfgGatePlan(image.Module(),plan),"mutate-before-commit fixture valid");image.bytes[offset]^=1;
    Check(!p::ValidateMfgGatePlan(plan),"changed semantic witness invalidates plan");std::vector<p::internal::BytePatch> changes;
    Check(!p::ApplyMfgGatePlan(plan,changes)&&changes.empty()&&provider_mock::protection_calls==0,"TOCTOU never writes");image.bytes=original;image.Bind();
  }
  Reset();image.Bind();p::FindMfgGatePlan(image.Module(),plan);p::g_create_seen=true;
  Check(!p::ApplyMfgGatePlan(plan,undo),"no live commit after Create");p::g_create_seen=false;
  Reset();Image ambiguous(3,true);Check(!p::FindMfgGatePlan(ambiguous.Module(),plan)&&plan.contract.result==mfgunlock::mfggate::Result::kAmbiguous,"duplicate semantic plans ambiguous");
  Reset();Image bad;
  auto negative=[&](size_t at,unsigned char value,const char* why){const auto keep=bad.bytes[at];bad.bytes[at]=value;bad.Bind();Check(!p::FindMfgGatePlan(bad.Module(),plan),why);Check(provider_mock::protection_calls==0,"negative discovery is read only");bad.bytes[at]=keep;};
  negative(0,'X',"malformed DOS");negative(0x80,'X',"malformed PE");negative(0x80+24+140,1,"truncated pdata table");
  negative(bad.cap_branch,0x74,"unrelated conditional branch");negative(bad.cap_branch+1,1,"branch into instruction");negative(bad.cap_site-1,0xfe,"unrelated architecture register");
  negative(bad.run_site+5,0x94,"wrong SETcc condition");negative(bad.flag_load+3,0x30,"unrelated field not paired");negative(bad.ceiling_mov+1,7,"advertise and execution limit mismatch");
  negative(0x4500,0xc3,"unrelated helper must not supply architecture");negative(0x4561,0x90,"corrupted enum normalization rejected");negative(0x3000,0xc3,"unreachable Evaluate sites");
  negative(0x1000,0xc3,"unreachable capability sink");negative(0x2000,0xc3,"unreachable Create gate");
  negative(0x5a00+120,8,"wrong CFG dispatch pointer rejected");
  negative(0x80+24+196,64,"truncated load config rejected");
  negative(0x2000+2,0xfa,"wrong this alias rejected");
  negative(0x4000+7,0x41,"count no longer loaded from same input rejected");
  // Reuse the same mapping with another identity. Cached plans must fail even
  // if the new module later happens to qualify independently.
  Reset();Image reload;p::MfgGatePlan oldplan,newplan;
  Check(p::FindMfgGatePlan(reload.Module(),oldplan),"reload base qualifies");
  reload.bytes[0x88]^=0x7f;
  Check(!p::ValidateMfgGatePlan(oldplan),"same base changed identity invalidates cached plan");
  Check(p::FindMfgGatePlan(reload.Module(),newplan),"new identity independently qualifies");
  Reset();Image fault; p::FindMfgGatePlan(fault.Module(),plan);provider_mock::retain_ok=false;
  Check(!p::ApplyMfgGatePlan(plan,undo)&&provider_mock::protection_calls==0,"failed retain denies writes");
  Reset();bad.Bind();
  // Discovery alone never writes, even when mutated inputs retain a recognizable
  // contract. Metamorphic fuzz checks bounds, determinism and validation.
  for(unsigned n=0;n<128;++n){
    Reset();Image fuzz;const size_t at=0x1000+(n*37)%0x3720;fuzz.bytes[at]^=static_cast<unsigned char>(1u<<(n%8));
    p::MfgGatePlan a,b;const bool fa=p::FindMfgGatePlan(fuzz.Module(),a),fb=p::FindMfgGatePlan(fuzz.Module(),b);
    Check(fa==fb&&a.contract.reason==b.contract.reason,"mutated image determinism");
    Check(provider_mock::protection_calls==0,"fuzz discovery read only");
    if(fa)Check(p::ValidateMfgGatePlan(a),"accepted mutation validates current witnesses");
  }
  Reset();bad.Bind();
  // An unrelated discarded section need not remain mapped after LoadLibrary.
  Reset();Image discarded;discarded.bytes.resize(0x9000);discarded.U32(0x80+24+56,0x9000);
  WORD section_count=3;std::memcpy(discarded.bytes.data()+0x86,&section_count,2);
  IMAGE_SECTION_HEADER dead{};dead.VirtualAddress=0x8000;dead.Misc.VirtualSize=0x1000;dead.Characteristics=IMAGE_SCN_MEM_READ;
  std::memcpy(discarded.bytes.data()+0x80+sizeof(IMAGE_NT_HEADERS64)+2*sizeof(dead),&dead,sizeof(dead));
  discarded.Bind();provider_mock::regions[0].bytes=0x8000;
  Check(p::FindMfgGatePlan(discarded.Module(),plan),"discarded unrelated section does not veto host MFG");
  Reset();bad.Bind();
  // A decoy raw cmp in a non-executable section is never nominated.
  const unsigned char decoy[]={0x81,0xfd,0xb0,1,0,0,0x7c,7};std::copy(std::begin(decoy),std::end(decoy),bad.bytes.begin()+0x5700);
  Check(p::FindMfgGatePlan(bad.Module(),plan),"noncode decoy ignored");
  // Systematic truncated image/section tests exercise bounded reads.
  for(unsigned n=0;n<512;n+=7){Reset();Image small;small.bytes.resize(n);small.Bind();Check(!p::FindMfgGatePlan(small.Module(),plan),"truncated image never parses");}
  std::cout<<"PASS "<<checks<<" checks\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
