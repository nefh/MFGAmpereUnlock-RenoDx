// SPDX-License-Identifier: MIT
// Production storage transaction with synthetic PE/PTX and Windows API doubles.
// No NVIDIA image, CUDA code or vendor API is executed.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <iostream>
#include <random>
#include <stdexcept>

using Bytes = std::vector<unsigned char>;
namespace p = mfgunlock::provider;
namespace f = mfgunlock::fatbin;
namespace t = mfgunlock::ptx;
namespace e = mfgunlock::endpoint;
namespace m = provider_mock;
unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class T> void Put(Bytes& b,size_t at,T value) {std::memcpy(b.data()+at,&value,sizeof(value));}
void Code(Bytes& b,size_t at,std::initializer_list<unsigned char> code) {
  std::copy(code.begin(),code.end(),b.begin()+at);
}
void Rel(Bytes& b,size_t at,size_t displacement,size_t length,size_t dest) {
  Put<int32_t>(b,at+displacement,static_cast<int32_t>(dest)-static_cast<int32_t>(at+length));
}
void Reset() {
  Check(m::allocations.empty(),"previous test leaked owned payload");
  p::internal::g_providers.clear();p::internal::g_rejected.clear();
  p::internal::g_registry_failed=false;p::internal::g_comparison_restore_failed=false;
  mfgunlock::g_enabled=true;mfgunlock::architecture::Configure(mfgunlock::Architecture::kTuring);
  p::g_create_seen=false;
  m::regions.clear();m::paths.clear();m::exports.clear();m::fail_protection_calls.clear();
  m::fail_protection_call=0;m::protection_calls=0;m::frees=0;m::fail_free=false;
  m::force_lock_busy=false;m::retain_ok=true;
}
Bytes TightContainer(uint32_t source_sm=89) {
  for(unsigned count: {1u,4u,16u,32u,64u,128u}) {
    std::string text=".version 8.2\n.target sm_"+std::to_string(source_sm)+"\n.address_size 64\n"
        ".visible .entry synthetic_storage() {\n.reg .b16 %rs<64>;\n.reg .f32 %f<8>;\n.reg .b32 %r<64>;\n";
    for(unsigned i=0;i<count;++i) text+=(i%2?"min.f16 ":"max.f16 ")+std::string("%rs")+
        std::to_string((i*7)%64)+", %rs"+std::to_string((i*11+1)%64)+", %rs"+
        std::to_string((i*19+2)%64)+";\n";
    text+="ret;\n}\n";text.push_back(0);
    Bytes compressed;Check(f::Lz4BlockCompress(text,65536,compressed),"compress synthetic PTX");
    Bytes b(80+((compressed.size()+7)&~size_t{7}));
    Put<uint32_t>(b,0,f::kMagic);Put<uint16_t>(b,4,1);Put<uint16_t>(b,6,16);
    Put<uint64_t>(b,8,b.size()-16);Put<uint16_t>(b,16,1);Put<uint16_t>(b,18,0x101);
    Put<uint32_t>(b,20,64);Put<uint64_t>(b,24,b.size()-80);
    Put<uint32_t>(b,32,compressed.size());Put<uint32_t>(b,44,source_sm);
    Put<uint64_t>(b,56,0x2041);Put<uint64_t>(b,72,text.size());
    std::copy(compressed.begin(),compressed.end(),b.begin()+80);
    t::Plan plan;std::string why;
    const auto result=t::Retarget(b,plan,why,mfgunlock::architecture::kTuring);
    Check(result==t::Result::kRetargeted,why.c_str());
    if(plan.requires_external_storage)return b;
  }
  throw std::runtime_error("synthetic program did not exercise overflow path");
}
struct Image {
  Bytes b=Bytes(0x10000);
  Bytes original;
  std::vector<e::ImageRange> ranges{{0x1000,0x1000,true},{0x4000,0xc000,false}};
  size_t container_bytes=0;
  uint32_t source_sm=89;
  static constexpr size_t payload=0x8000,loader=0x1200,name=0x7100;
  void* Module() {return b.data();}
  explicit Image(uint32_t sm=89):source_sm(sm) {
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;Put(b,0,dos);
    IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt.FileHeader.NumberOfSections=2;
    nt.FileHeader.TimeDateStamp=0x12345678;nt.FileHeader.SizeOfOptionalHeader=sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage=b.size();nt.OptionalHeader.SizeOfHeaders=0x400;
    nt.OptionalHeader.NumberOfRvaAndSizes=16;nt.OptionalHeader.DataDirectory[3]={0x4200,24};
    Put(b,0x80,nt);
    IMAGE_SECTION_HEADER text{};text.VirtualAddress=0x1000;text.Misc.VirtualSize=0x1000;
    text.Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    IMAGE_SECTION_HEADER data{};data.VirtualAddress=0x4000;data.Misc.VirtualSize=0xc000;
    data.Characteristics=IMAGE_SCN_MEM_READ;
    Put(b,0x80+sizeof(nt),text);Put(b,0x80+sizeof(nt)+sizeof(text),data);
    Code(b,0x1000,{0xb8,0x90,1,0,0,0xc3});Code(b,0x1100,{0xc3});
    const auto container=TightContainer(source_sm);container_bytes=container.size();
    std::copy(container.begin(),container.end(),b.begin()+payload);
    std::memcpy(b.data()+name,"synthetic_storage",18);
    std::memcpy(b.data()+0x7000,"dlfg_kernel",12);
    Code(b,loader,{0x4c,0x8d,0x0d,0,0,0,0,0x41,0xb8,0,0,0,0,
        0x48,0x8d,0x15,0,0,0,0,0x48,0x8b,0xcb,0xe8,0,0,0,0,0xc3});
    Rel(b,loader,3,7,name);Put<uint32_t>(b,loader+9,container_bytes);
    Rel(b,loader+13,3,7,payload);Rel(b,loader+23,1,5,0x1100);
    Put<uint32_t>(b,0x4200,0x1100);Put<uint32_t>(b,0x4204,0x1101);
    Put<uint32_t>(b,0x420c,loader);Put<uint32_t>(b,0x4210,loader+29);
    m::regions.push_back({Module(),b.size()});m::paths[Module()]="X:\\fixture\\nvngx_dlssg.dll";
    m::exports[{Module(),"NVSDK_NGX_GetGPUArchitecture"}]=reinterpret_cast<FARPROC>(b.data()+0x1000);
    m::exports[{Module(),"NVSDK_NGX_D3D12_PopulateParameters_Impl"}]=reinterpret_cast<FARPROC>(b.data()+0x1100);
    original=b;
  }
  bool Loads(std::vector<mfgunlock::storage::LoadSite>& sites,std::string& why) {
    e::structural::ImageView view{b,ranges,{}};
    return view.ReadFunctions() && mfgunlock::storage::FindLoads(view,payload,container_bytes,sites,why,source_sm);
  }
};
void Successful(uint32_t source_sm=89) {
  Reset();Image x(source_sm);
  Check(p::PrepareProvider(x.Module()),"larger PTX published");
  Check(m::allocations.size()==1,"one owned allocation");
  auto& record=p::internal::g_providers.back();
  Check(record.overflow_storage.size()==1,"owned storage registered");
  const auto& storage=record.overflow_storage[0];
  Check(storage.allocation_bytes>x.container_bytes,"replacement actually exceeds original storage");
  Check(f::ReadU64(x.b.data()+x.payload)==reinterpret_cast<uintptr_t>(storage.allocation),"pointer cell published");
  Check(x.b[x.loader+14]==0x8b,"LEA to MOV load indirection");
  Check(f::ReadU32(x.b.data()+x.loader+9)==storage.allocation_bytes,"loader size updated");
  Check(e::structural::PtxHasEntry({static_cast<unsigned char*>(storage.allocation),storage.allocation_bytes},75,
                                  "synthetic_storage"),"replacement has named SM75 entry");
  MEMORY_BASIC_INFORMATION mbi{};VirtualQuery(storage.allocation,&mbi,sizeof(mbi));
  Check(mbi.Protect==PAGE_READONLY,"replacement is read only");
  const auto calls=m::protection_calls;
  for(unsigned n=0;n<10;++n) Check(p::PrepareProvider(x.Module()),"idempotent prepare");
  Check(m::protection_calls==calls && m::allocations.size()==1,"repeat performs no write/allocation");
  x.b[x.loader+9]^=1;
  Check(!p::internal::StoragePublished(record),"modified length invalidates readiness");
  x.b[x.loader+9]^=1;
  x.b[x.payload]^=1;
  Check(!p::internal::StoragePublished(record),"modified pointer invalidates readiness");
  x.b[x.payload]^=1;
  Check(p::internal::StoragePublished(record),"restored state recognized");
  p::Restore();Check(x.b==x.original,"byte-identical restore");
  Check(m::allocations.empty() && m::frees==1,"released after all aliases restored");
}
// A failure scheduled beyond Prepare's actual calls did not occur. Do not
// carry it into the unrelated cleanup transaction (heap pointer bytes can
// change the number of Prepare writes). Restore-failure tests below keep their
// own explicit injections and retain/retry assertions.
void CleanupAfterUnreachedPrepareFault() {
  m::fail_protection_call=0;
  p::Restore();
}
void FailurePaths() {
  // Every protection failure during prepare must restore the complete image.
  Reset();unsigned calls=0;
  {
    Image x;Check(p::PrepareProvider(x.Module()),"baseline prepare");calls=m::protection_calls;
    p::Restore();Check(m::allocations.empty(),"baseline freed");
  }
  for(unsigned failure=1;failure<=calls;++failure) {
    Reset();Image x;m::fail_protection_call=failure;
    const bool prepared=p::PrepareProvider(x.Module());
    // Actual pointer bytes can coincide with old header bytes, reducing the
    // number of byte writes on another heap layout. A beyond-end injection
    // must not be mistaken for an unhandled protection failure.
    if(prepared) {
      Check(m::protection_calls<failure,"injected protection failure detected");
      CleanupAfterUnreachedPrepareFault();
    }
    Check(x.b==x.original,"failed prepare rolled back bytes");
    Check(m::allocations.empty(),"failed prepare released unpublished or restored memory");
  }
  Reset();
  {
    Image x;Check(p::PrepareProvider(x.Module()),"unreached fault fixture prepared");
    // Deterministic counterpart of the address-dependent beyond-end case.
    m::fail_protection_call=m::protection_calls+1;
    CleanupAfterUnreachedPrepareFault();
    Check(x.b==x.original && m::allocations.empty(),"prepare fault must not leak into cleanup");
  }
  Reset();
  {
    Image x;Check(p::PrepareProvider(x.Module()),"retain fixture prepared");
    m::fail_protection_call=m::protection_calls+1;
    p::Restore();Check(!m::allocations.empty(),"failed undo retains reachable allocation");
    m::fail_protection_call=0;p::Restore();
    Check(x.b==x.original && m::allocations.empty(),"retry undo restores and releases");
  }
  Reset();
  {
    Image x;Check(p::PrepareProvider(x.Module()),"free failure fixture prepared");
    m::fail_free=true;p::Restore();
    Check(x.b==x.original && !m::allocations.empty(),"failed free retains owner after alias restoration");
    m::fail_free=false;p::Restore();Check(m::allocations.empty(),"free retry releases allocation");
  }
}
void Refusals() {
  Reset();Image x;std::string why;std::vector<mfgunlock::storage::LoadSite> sites;
  Check(x.Loads(sites,why) && sites.size()==1,"direct tuple proven");
  const auto no_writes=[&](){Check(m::protection_calls==0 && m::allocations.empty(),"refused plan writes nothing");};
  for(unsigned variant=0;variant<8;++variant) {
    x.b=x.original;
    switch(variant) {
      case 0: Put<uint32_t>(x.b,x.loader+9,x.container_bytes+1);break;
      case 1: x.b[x.name]='z';break;
      case 2: Rel(x.b,x.loader+13,3,7,x.payload+1);break;
      case 3: Code(x.b,0x1400,{0x48,0x8d,0x05,0,0,0,0});Rel(x.b,0x1400,3,7,x.payload);break;
      case 4: Put<uint64_t>(x.b,0x5000,reinterpret_cast<uintptr_t>(x.Module())+x.payload+16);break;
      case 5: Code(x.b,x.loader+20,{0x48,0x8b,0xd3});break; // clobber RDX
      case 6: Rel(x.b,x.loader+23,1,5,0x1101);break; // not a function start
      case 7: Code(x.b,x.loader+20,{0xeb,1,0x90});break;
    }
    const auto before=x.b;
    Check(!x.Loads(sites,why) && sites.empty(),"unknown alias or incomplete tuple refused");
    Check(!p::PrepareProvider(x.Module()),"provider respects storage proof failure");
    Check(x.b==before,"refusal has no partial image changes");no_writes();
  }
  x.b=x.original;
  std::mt19937 random(0x751002);
  for(unsigned i=0;i<1024;++i) {
    x.b=x.original;x.b[x.loader+(random()%29)]^=static_cast<unsigned char>(1+random()%255);
    const auto before=x.b;(void)x.Loads(sites,why);
    Check(before==x.b,"loader mutation scan is read only");no_writes();
  }
}
int main() {
  try {Successful();Successful(120);FailurePaths();Refusals();Reset();
    std::cout<<"SM75 storage checks="<<checks<<" PASS\n";
  } catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
