// SPDX-License-Identifier: MIT
// Independent synthetic PE/CFG fixtures. No vendor code, DLL IDs or RVA table.
#include "mfg_gate_fixture.hpp"
#include <iostream>
using namespace mfg_gate_fixture;
namespace mg=mfgunlock::mfggate;
namespace p=mfgunlock::provider;
static unsigned checks=0,variants=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
struct Flow:Image {
  uint32_t pos=0,cmp_ad=0,cmp_exec=0,warp=0,cap_key=0,exec_slot=0,high_imm=0,index_imm=0;
  uint32_t low_imm=0,cmp_condition=0,flag_gap=0,ad_setter=0,reject_jump=0,join=0,count_load=0;
  std::map<std::string,uint32_t> labels;std::vector<std::pair<uint32_t,std::string>> fixups;
  unsigned count=3,idx=14,arc=5; bool extra_index=true;
  void B(std::initializer_list<unsigned char>x){Put(pos,x);}
  void Rex(bool wide,unsigned r=0,unsigned b=0){auto rex=0x40|(wide?8:0)|(r>=8?4:0)|(b>=8?1:0);if(rex!=0x40)B({static_cast<unsigned char>(rex)});}
  void Mov(unsigned dst,unsigned src,bool wide=false){Rex(wide,dst,src);B({0x8b,static_cast<unsigned char>(0xc0+((dst&7)<<3)+(src&7))});}
  void Lit(unsigned reg,uint32_t x){Rex(false,0,reg);B({static_cast<unsigned char>(0xb8+(reg&7))});Imm(pos,x);}
  void Load(unsigned dst,unsigned base,unsigned disp,bool wide=false){Rex(wide,dst,base);B({0x8b,static_cast<unsigned char>(0x80+((dst&7)<<3)+(base&7))});if((base&7)==4)B({0x24});Imm(pos,disp);}
  void Cmp(unsigned reg,uint32_t x,bool large=false){Rex(false,0,reg);B({static_cast<unsigned char>(large?0x81:0x83),static_cast<unsigned char>(0xf8+(reg&7))});if(large)Imm(pos,x);else B({static_cast<unsigned char>(x)});}
  void Cmov(unsigned cc,unsigned dst,unsigned src){Rex(false,dst,src);B({0x0f,static_cast<unsigned char>(0x40+cc),static_cast<unsigned char>(0xc0+((dst&7)<<3)+(src&7))});}
  void Label(const std::string&n){labels[n]=pos;}
  void J(unsigned cc,const std::string&n){if(cc==16)B({0xe9});else B({0x0f,static_cast<unsigned char>(0x80+cc)});fixups.emplace_back(pos,n);Imm(pos,0);}
  void Dispatch(){B({0xff,0x15});Imm(pos,0x5800-(pos+4));}
  void Error(const std::string& n){Label(n);Lea(pos,8,0x5480);Lit(0,0xbad00005);B({0xc3});}
  void Table(){std::sort(functions.begin(),functions.end());for(size_t n=0;n<functions.size();++n){U32(0x6000+n*12,functions[n].first);U32(0x6004+n*12,functions[n].second);U32(0x6008+n*12,0x5900);}U32(0x80+24+140,static_cast<uint32_t>(functions.size()*12));}
  explicit Flow(unsigned variant=0,unsigned ceiling=3):Image(ceiling) {
    const unsigned shift=(variant%7)*32, cc=(variant/7)%4, ccmap[]={12,13,2,3};
    const bool cmov=(variant/28)%2==0,alias=(variant/56)%2,limitreg=(variant/112)%2;
    extra_index=(variant/224)%2==0;
    const unsigned ds=alias?10:8,src=3;arc=(variant%3)==0?5:(variant%3)==1?7:12;
    count=(variant%2)?12:3;idx=(variant%2)?15:14;
    functions.clear();Func(0x4500,0x4720);
    std::fill(bytes.begin()+0x1000,bytes.begin()+0x4400,0);
    Named(0x5480,"EndpointCore::ValidateMultiFrameCount");Named(0x5500,"DLSSG.ReflexWarp.Available");
    const uint32_t cap=pos=0x1000+shift;
    Mov(14,2,true);B({0x48,0x8b,0x01,0x48,0x8b,0x40,0x38});Dispatch();Mov(1,0);Call(pos,0x4500);Mov(arc,0);
    // An unrelated failure-path write must not invalidate reaching definitions.
    if(variant%2){B({0x85,0xff});J(4,"cap_error");}
    B({0x49,0x8b,0x06});
    if(cmov){Lit(ds,(cc==0||cc==2)?ceiling:1);Lit(src,(cc==0||cc==2)?1:ceiling);}
    cmp_ad=pos;Cmp(arc,0x1b0,true);cmp_condition=pos;
    if(cmov){flag_gap=pos;B({0x90,0x90});Cmov(ccmap[cc],ds,src);}
    else {
      J(ccmap[cc],(cc==0||cc==2)?"ad_low":"ad_high");
      Label((cc==0||cc==2)?"ad_high":"ad_low");Lit(ds,(cc==0||cc==2)?ceiling:1);J(16,"ad_join");
      Label((cc==0||cc==2)?"ad_low":"ad_high");Lit(ds,(cc==0||cc==2)?1:ceiling);Label("ad_join");
    }
    if(alias)Mov(8,ds);
    cap_key=pos;Lea(pos,2,0x5100);Mov(1,14,true);ad_setter=pos;B({0x48,0x8b,0x40,0x20});Dispatch();
    warp=pos;Cmp(arc,0x1b0,true);B({0x40,0x0f,0x9d,0xc5});Lea(pos,2,0x5500);B({0xc3});
    if(variant%2){Label("cap_error");Lit(arc,12345);B({0xc3});}
    Func(cap,pos);
    const uint32_t eval=pos=0x3000+shift;
    Mov(6,1,true);Lea(pos,9,0x5300);count_load=pos;Load(count,4,0x40);Load(idx,4,0x44);
    Load(1,6,0x20,true);B({0x48,0x8b,0x01});exec_slot=pos+3;B({0x48,0x8b,0x40,0x38});Dispatch();
    cmp_exec=pos;B({0x3d});Imm(pos,0x1b0);J(ccmap[cc],(cc==0||cc==2)?"low":"high");
    J(16,(cc==0||cc==2)?"high":"low");
    Label("high");
    if(limitreg){Lit(10,ceiling);Rex(false,count,10);B({0x3b,static_cast<unsigned char>(0xc0+((count&7)<<3)+2)});}
    else {high_imm=pos+(count>=8?3:2);Cmp(count,ceiling);}
    if(variant%2){J(7,"high_error");J(16,"success");}
    else {J(6,"success");J(16,"high_error");}
    Label("low");low_imm=pos+(count>=8?3:2);Cmp(count,1);
    if(variant%2){J(5,"low_error");J(16,extra_index?"index":"success");}
    else {J(4,extra_index?"index":"success");J(16,"low_error");}
    Label("index");index_imm=pos+(idx>=8?3:2);Cmp(idx,1);J(4,"success");J(16,"index_error");
    Error("high_error");Error("low_error");Error("index_error");
    Label("success");join=pos;Lit(0,1);B({0xc3});Func(eval,pos);
    for(const auto&[at,n]:fixups) U32(at,labels.at(n)-(at+4));
    Table();Bind();
  }
};
// Intermediate topology: CMOV advertisement + Create flag + helper with an
// immediate/register bound and independently scheduled index loads.
struct Helper:Flow {
  uint32_t h_high=0,h_low=0,h_index=0;
  explicit Helper(unsigned variant,unsigned ceiling):Flow(variant,ceiling) {
    Image original(ceiling);
    std::copy(original.bytes.begin()+0x2000,original.bytes.begin()+0x4000,bytes.begin()+0x2000);
    auto cap=functions.front();functions.clear();functions.push_back(cap);
    for(auto f:original.functions)if(f.first==0x2000||f.first==0x3000||f.first==0x4500)functions.push_back(f);
    Named(0x5480,"EndpointCoreInputs::ComputeAndValidateTimeFactor");
    labels.clear();fixups.clear();pos=0x4000;
    const unsigned cr=(variant%2)?8:11;
    B({0x48,0x83,0xec,0x38});Load(cr,1,0x40);Mov(9,1,true);B({0x84,0xd2});
    J((variant%2)?5:4,(variant%2)?"high":"low");J(16,(variant%2)?"low":"high");
    Label("high");
    if((variant/2)%2){Lit(10,ceiling);Rex(false,cr,10);B({0x3b,static_cast<unsigned char>(0xc0+((cr&7)<<3)+2)});}
    else {h_high=pos+(cr>=8?3:2);Cmp(cr,ceiling+((variant/4)%2));}
    const unsigned hcc=(variant/4)%2&&!(variant/2%2)?2:6;
    J(hcc,"load_index");J(16,"high_error");
    Label("load_index");Load(1,1,0x44);Label("success");Lit(0,1);B({0x48,0x83,0xc4,0x38,0xc3});
    Label("low");h_low=pos+(cr>=8?3:2);Cmp(cr,1);J(4,"index");J(16,"low_error");
    Label("index");Load(1,1,0x44);h_index=pos+2;Cmp(1,1);J(4,"success");J(16,"index_error");
    Error("high_error");Error("low_error");Error("index_error");
    Func(0x4000,pos);for(const auto& [at,n]:fixups)U32(at,labels.at(n)-(at+4));Table();Bind();
  }
};
int main(){try{
  // Independent combinations: polarity, register allocation, materialized bound,
  // branch vs CMOV, layout shifts, extra frame-index condition, changing ceiling.
  for(unsigned v=0;v<448;++v) {
    Reset();const unsigned ceiling=(v%4)==0?3:(v%4)==1?5:(v%4)==2?7:11;
    Flow f(v,ceiling);p::MfgGatePlan plan;
    const bool ok=p::FindMfgGatePlan(f.Module(),plan);
    if(!ok)std::cerr<<"variant "<<v<<" reason "<<plan.contract.reason<<"\n";
    Check(ok,"generic layout qualifies");Check(plan.contract.ceiling==ceiling,"limit derived from current code");
    Check(plan.contract.sites[0].instruction==f.cmp_ad&&plan.contract.sites[1].instruction==f.cmp_exec,"only the paired count gates selected");
    Check(provider_mock::protection_calls==0,"discovery is read only");
    const auto original=f.bytes;
    for(auto arch:{mfgunlock::Architecture::kAmpere,mfgunlock::Architecture::kAda,mfgunlock::Architecture::kTuring}) {
      Reset();f.Bind();mfgunlock::architecture::Configure(arch);p::MfgGatePlan current;
      Check(p::FindMfgGatePlan(f.Module(),current),"all targets use same structural analysis");
      std::vector<p::internal::BytePatch> undo;Check(p::ApplyMfgGatePlan(current,undo),"real paired writer commits");
      Check(f.bytes[f.warp+2]==original[f.warp+2],"unrelated Warp predicate unchanged");
      Check(f.bytes[f.cmp_exec+1]==static_cast<unsigned char>(mfgunlock::architecture::ActiveProfile()->native_arch),"target threshold used");
      Check(p::RestoreMfgComparisons(undo)&&f.bytes==original,"exact rollback");
    }
    ++variants;
  }
  for(unsigned v=0;v<64;++v) {
    Reset();Helper h(v,3+2*(v%4));p::MfgGatePlan plan;
    const bool ok=p::FindMfgGatePlan(h.Module(),plan);
    if(!ok)std::cerr<<"helper variant "<<v<<" reason "<<plan.contract.reason<<"\n";
    Check(ok,"general helper count topology qualifies");
    Check(plan.contract.ceiling==3+2*(v%4),"helper and advertisement bound paired");
    Check(plan.contract.reason=="ADVERTISE_EXECUTE_COUNT_CONTRACT","Create flag pathway retained");
    const auto before=h.bytes;std::vector<p::internal::BytePatch> undo;
    Check(p::ApplyMfgGatePlan(plan,undo),"helper paired commit");
    Check(p::RestoreMfgComparisons(undo)&&h.bytes==before,"helper paired restore");
    ++variants;
  }
  for(unsigned mutation=0;mutation<3;++mutation){
    Reset();Helper h(0,3);
    const auto at=mutation==0?h.h_high:mutation==1?h.h_low:h.h_index;
    h.bytes[at]=2;p::MfgGatePlan plan;
    Check(!p::FindMfgGatePlan(h.Module(),plan),"mismatched helper/count/index contract rejected");
    Check(provider_mock::protection_calls==0,"invalid helper never writes");
  }
  auto negative=[](const auto& change,const char* why){Reset();Flow f;change(f);f.Bind();p::MfgGatePlan p;Check(!p::FindMfgGatePlan(f.Module(),p),why);Check(provider_mock::protection_calls==0,"negative never writes");};
  negative([](Flow& f){f.bytes[f.flag_gap]=0x85;f.bytes[f.flag_gap+1]=0xc0;},"intervening TEST destroys flags");
  negative([](Flow& f){f.bytes[f.cmp_condition+4]=0x44;},"equal-only CMOV is not architecture threshold");
  negative([](Flow& f){f.bytes[f.exec_slot]=0x40;},"different architecture query not paired");
  negative([](Flow& f){f.bytes[f.high_imm]=2;},"different count ceiling not patched");
  negative([](Flow& f){f.bytes[f.low_imm]=2;},"low branch must accept exactly one");
  negative([](Flow& f){f.bytes[f.index_imm]=2;},"index check remains native");
  negative([](Flow& f){f.bytes[f.ad_setter+3]=0x18;},"bool setter is not count setter");
  negative([](Flow& f){f.bytes[0x5100]='X';},"different capability string not accepted");
  negative([](Flow& f){f.bytes[0x5480]='X';},"unrelated execution checks not accepted");
  negative([](Flow& f){f.bytes[f.count_load]=0xb8;},"count not from input field");
  negative([](Flow& f){f.U32(f.cmp_ad+2,0x180);},"different feature architecture compare");
  negative([](Flow& f){auto at=f.labels.at("high_error");f.bytes[at]=0xe9;f.U32(at+1,f.join-(at+5));},"error path reaching success cannot authorize patch");
  negative([](Flow& f){auto at=f.labels.at("high_error")+7;f.bytes[at]=0xe9;f.U32(at+1,static_cast<uint32_t>(-5));},"nonterminating error path is not a rejection");
  // Cache TOCTOU and the writer's rollback behavior also apply to new layouts.
  for(unsigned which=0;which<4;++which){Reset();Flow f;p::MfgGatePlan plan;Check(p::FindMfgGatePlan(f.Module(),plan),"TOCTOU initial plan");const auto at=which==0?f.cmp_ad:which==1?f.cmp_exec:which==2?0x5480:0x4500;f.bytes[at]^=1;Check(!p::ValidateMfgGatePlan(plan),"semantic witness mutation rejected");}
  for(unsigned n=1;n<=4;++n){Reset();Flow f;const auto original=f.bytes;p::MfgGatePlan plan;Check(p::FindMfgGatePlan(f.Module(),plan),"rollback initial plan");provider_mock::fail_protection_call=n;std::vector<p::internal::BytePatch> undo;Check(!p::ApplyMfgGatePlan(plan,undo),"failed memory protection does not half-patch");Check(f.bytes==original,"rollback retained original bytes");}
  std::cout<<"PASS "<<checks<<" checks, "<<variants<<" structural variants\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
