// SPDX-License-Identifier: MIT
// Independent miniature legacy-flat contract. No vendor bytes/known RVAs are
// used; the two API networks each have one generated PTX kernel. Test runtime
// preparation/rollback against the real provider header and memory doubles.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
namespace e=mfgunlock::endpoint;
namespace p=mfgunlock::provider;
namespace f=mfgunlock::fatbin;
using Bytes=std::vector<unsigned char>;
static unsigned checks=0;
void Check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
template<class T> void Put(Bytes& b,size_t at,T x){std::memcpy(b.data()+at,&x,sizeof(x));}
struct Fixture {
  Bytes b=Bytes(0x18000);
  uint32_t code=0x1000,data=0x9000;
  std::vector<e::ImageRange> ranges{{0x1000,0x7000,true},{0x8000,0x10000,false}};
  std::vector<std::pair<uint32_t,uint32_t>> functions;
  std::array<uint32_t,2> selectors{},ptxloader{},native_ctor{},ptx_ctor{},ptx_table{},native_table{},payload{},length_operand{};
  uint32_t guard=0x8700,helper=0,init=0,stub=0,wrapper=0,wrapper_table=0,destructor=0;
  unsigned at=0;
  uint32_t fresh(){code=(code+31)&~31u;at=code;return at;}
  void bytes(std::initializer_list<unsigned char> s){std::copy(s.begin(),s.end(),b.begin()+at);at+=static_cast<unsigned>(s.size());}
  void end(uint32_t start){functions.emplace_back(start,at);code=at;}
  void rel(uint32_t pos,unsigned off,unsigned len,uint32_t dest){Put<int32_t>(b,pos+off,static_cast<int32_t>(dest)-static_cast<int32_t>(pos+len));}
  void call(uint32_t dest){auto pos=at;bytes({0xe8,0,0,0,0});rel(pos,1,5,dest);}
  void indirect(){auto pos=at;bytes({0xff,0x15,0,0,0,0});rel(pos,2,6,guard);}
  uint32_t branch(unsigned char op){auto pos=at;bytes({op,0});return pos;}
  void jump8(uint32_t pos,uint32_t dest){Check(static_cast<int>(dest)-static_cast<int>(pos+2)>=-128&&static_cast<int>(dest)-static_cast<int>(pos+2)<=127,"fixture short branch");b[pos+1]=static_cast<unsigned char>(dest-pos-2);}
  void lea(unsigned char prefix,unsigned char modrm,uint32_t dest){auto pos=at;bytes({prefix,0x8d,modrm,0,0,0,0});rel(pos,3,7,dest);}
  void imm(unsigned char op,uint32_t value){bytes({op,0,0,0,0});Put<uint32_t>(b,at-4,value);}
  uint32_t text(std::string_view s){auto pos=data;std::copy(s.begin(),s.end(),b.begin()+pos);data+=static_cast<uint32_t>(s.size()+1);data=(data+7)&~7u;return pos;}
  uint32_t table(std::string name,std::array<uint32_t,15> slots) {
    const auto type=data;data+=16;text(name);const auto col=data;data+=24;
    Put<uint32_t>(b,col,1);Put<uint32_t>(b,col+12,type);Put<uint32_t>(b,col+20,col);
    auto table=data+8;data+=8+15*8+8;
    Put<uint64_t>(b,table-8,reinterpret_cast<uintptr_t>(b.data())+col);
    for(unsigned i=0;i<slots.size();++i)Put<uint64_t>(b,table+i*8,reinterpret_cast<uintptr_t>(b.data())+slots[i]);
    return table;
  }
  uint32_t container(){
    std::string t=".version 8.7\n.target sm_89\n.address_size 64\n.visible .entry kernel() {ret;}\n";t.push_back(0);
    const auto pos=data;const auto compressed=t.size()+2;const auto n=80+((compressed+7)&~size_t{7});data+=static_cast<uint32_t>(n);
    Put<uint32_t>(b,pos,f::kMagic);Put<uint16_t>(b,pos+4,1);Put<uint16_t>(b,pos+6,16);Put<uint64_t>(b,pos+8,n-16);
    Put<uint16_t>(b,pos+16,1);Put<uint16_t>(b,pos+18,0x101);Put<uint32_t>(b,pos+20,64);Put<uint64_t>(b,pos+24,n-80);
    Put<uint32_t>(b,pos+32,static_cast<uint32_t>(compressed));Put<uint32_t>(b,pos+44,89);Put<uint64_t>(b,pos+56,0x2041);Put<uint64_t>(b,pos+72,t.size());
    b[pos+80]=0xf0;b[pos+81]=static_cast<unsigned char>(t.size()-15);std::copy(t.begin(),t.end(),b.begin()+pos+82);
    return pos;
  }
  uint32_t loader(uint32_t blob,uint32_t len,uint32_t name,uint32_t& sizearg){
    auto start=fresh();bytes({0x53,0x48,0x8b,0xd9});lea(0x4c,0x0d,name);lea(0x48,0x15,blob);
    bytes({0x41,0xb8,0,0,0,0});sizearg=at-4;Put<uint32_t>(b,sizearg,len);
    bytes({0x48,0x8b,0xcb});call(helper);bytes({0x48,0x89,0x83,0,2,0,0,0x5b,0xc3});end(start);return start;
  }
  uint32_t ctor(uint32_t table){auto start=fresh();bytes({0x53,0x48,0x8b,0xd9});lea(0x48,0x05,table);bytes({0x48,0x89,0x03,0x48,0x8b,0xc3,0x5b,0xc3});end(start);return start;}
  uint32_t arm(uint32_t ctor,uint32_t allocator){
    imm(0xb9,0x400);call(allocator);bytes({0x48,0x89,0x45,0xf0,0x48,0x85,0xc0});auto null=branch(0x74);
    bytes({0x48,0x8b,0xc8});call(ctor);bytes({0x48,0x8b,0xc8});auto live=branch(0xeb);
    jump8(null,at);bytes({0x33,0xc9});jump8(live,at);return at;
  }
  uint32_t pair(unsigned role,uint32_t allocator){
    auto query=at;indirect();bytes({0x85,0xc0});selectors[role]=branch(0x74);
    bytes({0x83,0xe8,1});auto j1=branch(0x74);bytes({0x83,0xe8,1});auto j2=branch(0x74);
    bytes({0x83,0xf8,1});auto j3=branch(0x75);jump8(j1,at);jump8(j2,at);
    arm(native_ctor[role],allocator);auto join=branch(0xeb);
    jump8(selectors[role],at);jump8(j3,at);arm(ptx_ctor[role],allocator);jump8(join,at);
    const auto field=static_cast<unsigned char>(0x28+role*8);
    bytes({0x48,0x89,0x4e,field,0x48,0x8b,0x4e,field,0x48,0x8b,0x01,0x45,0x33,0xc9,0x48,0x8b,0x40,0x10});indirect();return query;
  }
  explicit Fixture(uint32_t shift=0){
    code+=shift;data+=shift;
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;Put(b,0,dos);
    IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt.FileHeader.NumberOfSections=2;
    nt.FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);nt.FileHeader.TimeDateStamp=0x11223344+shift;
    nt.OptionalHeader.Magic=0x20b;nt.OptionalHeader.SizeOfImage=static_cast<uint32_t>(b.size());nt.OptionalHeader.SizeOfHeaders=0x400;nt.OptionalHeader.NumberOfRvaAndSizes=16;Put(b,0x80,nt);
    IMAGE_SECTION_HEADER sc{};sc.VirtualAddress=0x1000;sc.Misc.VirtualSize=0x7000;sc.Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;Put(b,0x188,sc);
    sc.VirtualAddress=0x8000;sc.Misc.VirtualSize=0x10000;sc.Characteristics=IMAGE_SCN_MEM_READ;Put(b,0x1b0,sc);
    Put<uint32_t>(b,0x98+192,0x8600);Put<uint32_t>(b,0x98+196,128);Put<uint32_t>(b,0x8600,128);Put<uint64_t>(b,0x8600+120,reinterpret_cast<uintptr_t>(b.data())+guard);
    stub=fresh();bytes({0xc3});end(stub);
    helper=fresh();bytes({0x48,0x8b,0x49,0x40,0x48,0x8b,0x01,0x48,0x8b,0x40,0x58});indirect();bytes({0xc3});end(helper);
    init=fresh();bytes({0x53,0x57,0x41,0x0f,0xb6,0xd9,0x48,0x8b,0xf9,0x84,0xdb});auto skip=branch(0x75);
    bytes({0x48,0x8b,0x07,0x48,0x8b,0xcf,0x48,0x8b,0x40,0x20});indirect();jump8(skip,at);bytes({0x5f,0x5b,0xc3});end(init);
    const auto name=text("kernel");
    for(unsigned role=0;role<2;++role){
      payload[role]=container();const auto original=data;data+=0x80;
      uint32_t ignored=0;const auto native_loader=loader(original,0x80,name,ignored);
      ptxloader[role]=loader(payload[role],static_cast<uint32_t>(f::ReadU64(b.data()+payload[role]+8)+16),name,length_operand[role]);
      std::array<uint32_t,15> slots;slots.fill(stub);slots[2]=init;slots[4]=native_loader;
      const std::string suffix=role==0?"@dx@@":"@vk@@";
      native_table[role]=table(".?AVFlatten_dlfg_bilerp"+suffix,slots);slots[4]=ptxloader[role];ptx_table[role]=table(".?AVFlatten_dlfg_bilerp_89_PTX"+suffix,slots);
      native_ctor[role]=ctor(native_table[role]);ptx_ctor[role]=ctor(ptx_table[role]);
    }
    destructor=fresh();bytes({0x53,0x48,0x8b,0xd9,0x48,0x8b,0x4b,0x30,0x48,0x85,0xc9});auto one=branch(0x75);
    bytes({0x48,0x8b,0x4b,0x28,0x48,0x85,0xc9});auto none=branch(0x74);jump8(one,at);
    bytes({0x48,0x8b,0x01,0xba,1,0,0,0,0x48,0x8b,0x00});indirect();jump8(none,at);bytes({0x5b,0xc3});end(destructor);
    wrapper=fresh();bytes({0x56,0x48,0x8b,0xf1,0x48,0x8b,0x4e,0x38,0x48,0x8b,0x01,0x48,0x8b,0x40,0x40,0x83,0x7d,0xe0,2});
    auto split=at;bytes({0x0f,0x85,0,0,0,0});pair(1,stub);auto finaljump=at;bytes({0xe9,0,0,0,0});
    rel(split,2,6,at);pair(0,stub);rel(finaljump,1,5,at);bytes({0x5e,0xc3});end(wrapper);
    std::array<uint32_t,15> slots;slots.fill(stub);slots[0]=destructor;slots[1]=wrapper;wrapper_table=table(".?AVDLFGNetworkWrapperFlat@@",slots);
    auto gate=fresh();bytes({0xb8,0x90,1,0,0,0xc3});end(gate);
    text("dlfg_kernel");
    std::sort(functions.begin(),functions.end());Put<uint32_t>(b,0x98+136,0x8000);Put<uint32_t>(b,0x98+140,static_cast<uint32_t>(functions.size()*12));
    for(size_t i=0;i<functions.size();++i){Put<uint32_t>(b,0x8000+i*12,functions[i].first);Put<uint32_t>(b,0x8004+i*12,functions[i].second);}
    provider_mock::regions.push_back({b.data(),b.size()});provider_mock::paths[b.data()]="X:\\synthetic\\nvngx_dlssg.dll";
    provider_mock::exports[{b.data(),"NVSDK_NGX_GetGPUArchitecture"}]=reinterpret_cast<FARPROC>(b.data()+gate);
    provider_mock::exports[{b.data(),"NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"}]=reinterpret_cast<FARPROC>(b.data()+stub);
  }
};
void Reset(){p::Restore();provider_mock::regions.clear();provider_mock::paths.clear();provider_mock::exports.clear();provider_mock::protection_calls=0;provider_mock::fail_protection_call=0;provider_mock::fail_protection_calls.clear();p::internal::g_registry_failed=false;p::g_create_seen=false;mfgunlock::g_enabled=true;mfgunlock::architecture::Configure(mfgunlock::Architecture::kTuring);}
int main(){try{
  for(unsigned shift:{0u,0x400u,0x800u}){
    Reset();Fixture x(shift);e::StructuralPlan plan;std::string why;
    Check(e::FindEndpointPlan(x.b,x.ranges,plan,why),why.c_str());
    Check(plan.family==e::Family::kLegacyFlat&&plan.payloads.size()==2,"flat family and derived counts");
    for(unsigned r=0;r<2;++r)Check(plan.selectors[r].rva==x.selectors[r]&&plan.selectors[r].after[0]==0xeb,"JE->JMP targets PTX arm");
    const auto before=x.b;Check(e::WitnessesMatch(x.b,plan),"witness before commit");
    x.b[x.init]^=1;Check(!e::WitnessesMatch(x.b,plan),"TOCTOU helper mutation");x.b[x.init]^=1;
    Check(p::PrepareProvider(x.b.data()),"production flat preparation");Check(p::internal::g_providers.front().endpoint_selected,"production endpoint ready");
    const auto writes=provider_mock::protection_calls;Check(p::PrepareProvider(x.b.data())&&provider_mock::protection_calls==writes,"idempotent no writes");
    x.b[x.selectors[0]]=0x74;Check(p::PreparedProviderCount()==0,"corrupted selector removes readiness");x.b[x.selectors[0]]=0xeb;
    p::Restore();Check(x.b==before,"complete rollback");
    // Reuse same module address with changed image identity and no stale plan.
    Put<uint32_t>(x.b,0x88,0x99887766);const auto replacement=x.b;
    Check(p::PrepareProvider(x.b.data()),"same address different identity prepares anew");p::Restore();Check(x.b==replacement,"replacement restores own bytes");
    for(unsigned fail:{1u,3u,7u}){
      provider_mock::protection_calls=0;provider_mock::fail_protection_call=fail;
      Check(!p::PrepareProvider(x.b.data()),"partial commit failure rejects");p::Restore();Check(x.b==replacement,"partial commit rollback");provider_mock::fail_protection_call=0;
    }
    provider_mock::retain_ok=false;
    Check(!p::PrepareProvider(x.b.data())&&x.b==replacement,"failed module retention cannot publish endpoint");
    provider_mock::retain_ok=true;
    Check(p::PrepareProvider(x.b.data()),"prepare before interrupted restore");
    provider_mock::fail_protection_call=provider_mock::protection_calls+1;
    p::Restore();
    Check(!p::internal::g_providers.empty()&&p::PreparedProviderCount()==0,"incomplete restore retains undo but never readiness");
    provider_mock::fail_protection_call=0;p::Restore();
    Check(x.b==replacement&&p::internal::g_providers.empty(),"restore retry recovers original bytes");
    Check(p::PrepareProvider(x.b.data()),"prepare before simulated unavailable mapping");
    const auto regions=provider_mock::regions;provider_mock::regions.clear();
    Check(p::PreparedProviderCount()==0,"missing mapping never considered selected");
    p::Restore();Check(!p::internal::g_providers.empty(),"unreadable mapping retains recovery metadata");
    provider_mock::regions=regions;p::Restore();Check(x.b==replacement,"recovered mapping can restore its own bytes");
    // Family transition after restore: a formerly selected address must not
    // inherit that plan when RTTI identifies a different, unproven contract.
    const auto col=static_cast<uint32_t>(f::ReadU64(x.b.data()+x.wrapper_table-8)-reinterpret_cast<uintptr_t>(x.b.data()));
    const auto type=f::ReadU32(x.b.data()+col+12);x.b[type+16]='X';
    Check(!e::FindEndpointPlan(x.b,x.ranges,plan,why)&&plan.family==e::Family::kUnknown,"same-address family change does not reuse endpoint metadata");
    x.b[type+16]='.';
    mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);
    Check(p::PrepareProvider(x.b.data()),"Ampere still prepares");for(auto at:x.selectors)Check(x.b[at]==0x74,"Ampere never uses flat redirect");p::Restore();
    mfgunlock::architecture::Configure(mfgunlock::Architecture::kAda);
    Check(!p::PrepareProvider(x.b.data())&&x.b==replacement,"Ada remains native unmodified");
  }
  Reset();Fixture x;e::StructuralPlan plan;std::string why;const auto original=x.b;
  const std::vector<size_t> mutations={0x80,0x98+136,x.selectors[0],x.selectors[0]+1,x.selectors[0]+4,
    x.ptx_ctor[0]+7,x.length_operand[0],x.ptx_table[0]+16,x.ptx_table[0]+40,x.wrapper_table-8,x.payload[0]+8,x.helper+10};
  for(const auto at:mutations){x.b[at]^=1;Check(!e::FindEndpointPlan(x.b,x.ranges,plan,why),"malformed/decoy contract rejected");Check(plan.family==e::Family::kUnknown&&plan.payloads.empty(),"no partial plan published");std::copy(original.begin(),original.end(),x.b.begin());}
  std::mt19937 rng(750602);for(unsigned n=0;n<128;++n){auto at=x.wrapper+(rng()%100);x.b[at]^=static_cast<unsigned char>(1+rng()%255);const auto before=x.b;(void)e::FindEndpointPlan(x.b,x.ranges,plan,why);Check(before==x.b,"fuzz-like discovery remains read-only");std::copy(original.begin(),original.end(),x.b.begin());}
  Reset();std::cout<<"endpoint_flat_test PASS checks="<<checks<<"\n";return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL "<<checks<<": "<<ex.what()<<"\n";return 1;}}
