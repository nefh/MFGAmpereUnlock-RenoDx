// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <iostream>
#include <stdexcept>
using Bytes=std::vector<unsigned char>;
namespace p=mfgunlock::provider;
namespace c=mfgunlock::cubin;
namespace f=mfgunlock::fatbin;
static unsigned checks=0;
void Check(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
template<class T>void Put(Bytes& b,size_t at,T x){std::memcpy(b.data()+at,&x,sizeof(x));}
Bytes Elf() {
  Bytes b(512);
  std::memcpy(b.data(),"\177ELF\2\1\1\63\7",9);
  Put<uint16_t>(b,16,2);Put<uint16_t>(b,18,190);Put<uint32_t>(b,20,118);
  Put<uint64_t>(b,40,256);Put<uint32_t>(b,48,0x590559);
  Put<uint16_t>(b,52,64);Put<uint16_t>(b,58,64);Put<uint16_t>(b,60,3);Put<uint16_t>(b,62,1);
  constexpr char names[]="\0.shstrtab\0.text.main_kernel\0";
  std::memcpy(b.data()+64,names,sizeof(names));
  const size_t str=320, text=384;
  Put<uint32_t>(b,str,1);Put<uint32_t>(b,str+4,3);Put<uint64_t>(b,str+24,64);Put<uint64_t>(b,str+32,sizeof(names));
  Put<uint32_t>(b,text,11);Put<uint32_t>(b,text+4,1);Put<uint64_t>(b,text+8,6);
  Put<uint64_t>(b,text+24,128);Put<uint64_t>(b,text+32,32);Put<uint64_t>(b,text+48,16);
  // Arbitrary instruction bytes. The test asserts no instruction rewriting;
  // it does not claim this synthetic blob is executable on a real GPU.
  for(size_t i=128;i<160;++i)b[i]=static_cast<unsigned char>(i);
  return b;
}
Bytes Fatbin(const Bytes& elf,bool plaintext=false) {
  Bytes raw(128,0x91);
  if(plaintext) {
    const std::string text=".version 8.7\n.target sm_89\n.address_size 64\n.visible .entry test() { ret; }\n";
    raw.assign(text.begin(),text.end());raw.push_back(0);
  }
  Bytes compressed;
  const std::string_view view(reinterpret_cast<const char*>(raw.data()),raw.size());
  Check(f::Lz4BlockCompress(view,1024,compressed),"fixture compression");
  const size_t padded=(compressed.size()+7)&~size_t{7};
  Bytes b(16+64+padded+64+elf.size());
  Put<uint32_t>(b,0,f::kMagic);Put<uint16_t>(b,4,1);Put<uint16_t>(b,6,16);Put<uint64_t>(b,8,b.size()-16);
  Put<uint16_t>(b,16,1);Put<uint16_t>(b,18,0x101);Put<uint32_t>(b,20,64);Put<uint64_t>(b,24,padded);
  Put<uint32_t>(b,32,static_cast<uint32_t>(compressed.size()));Put<uint32_t>(b,44,89);
  Put<uint64_t>(b,56,0x2041);Put<uint64_t>(b,64,0x98765432);Put<uint64_t>(b,72,raw.size());
  std::copy(compressed.begin(),compressed.end(),b.begin()+80);
  const auto e=80+padded;
  Put<uint16_t>(b,e,2);Put<uint16_t>(b,e+2,0x101);Put<uint32_t>(b,e+4,64);Put<uint64_t>(b,e+8,elf.size());
  Put<uint32_t>(b,e+28,89);Put<uint64_t>(b,e+40,0x41);std::copy(elf.begin(),elf.end(),b.begin()+e+64);
  return b;
}
int main() {
 try {
  std::string why;Bytes out;
  const auto elf=Elf();Check(c::InspectSm89(elf,why),"valid ELF layout");
  const auto good=Fatbin(elf);
  Check(c::RetargetOpaqueSm89To86(good,out,why),"native trial");
  std::vector<size_t> changes;for(size_t i=0;i<good.size();++i)if(good[i]!=out[i])changes.push_back(i);
  Check(changes.size()==3,"exactly three metadata bytes");
  for(size_t i:changes)Check(good[i]==89 && out[i]==86,"only target digits");
  const auto already_retargeted = out;
  Check(!c::RetargetOpaqueSm89To86(already_retargeted,out,why),"already native target not rewritten");
  Check(!c::RetargetOpaqueSm89To86(Fatbin(elf,true),out,why),"no fallback for plaintext PTX");
  auto bad=good;Put<uint64_t>(bad,64,0);Check(!c::RetargetOpaqueSm89To86(bad,out,why),"opaque tag required");
  for(size_t n=0;n<elf.size();++n) {
    const bool accepted=c::InspectSm89(std::span(elf).first(n),why);
    if(n<448)Check(!accepted,"truncated tables rejected");
  }
  for(const size_t at:{size_t{4},size_t{5},size_t{7},size_t{8},size_t{16},size_t{18},size_t{48},size_t{50},size_t{52},size_t{58},size_t{62}}) {
    auto x=elf;x[at]^=0x80;Check(!c::InspectSm89(x,why),"invalid ELF contract rejected");
  }
  auto x=elf;Put<uint64_t>(x,40,~uint64_t{0});Check(!c::InspectSm89(x,why),"overflow table offset");
  x=elf;Put<uint64_t>(x,384+32,31);Check(!c::InspectSm89(x,why),"instruction alignment");
  x=elf;Put<uint64_t>(x,384+24,48);Check(!c::InspectSm89(x,why),"code cannot alias metadata word");
  x=elf;Put<uint32_t>(x,384,0xffffff);Check(!c::InspectSm89(x,why),"section name bounds");
  x=elf;Put<uint64_t>(x,384+24,256);Check(!c::InspectSm89(x,why),"code cannot alias section table");
  uint32_t state=0x93258712;
  for(size_t i=0;i<2048;++i) {
    auto v=good;state=state*1664525u+1013904223u;const size_t at=state%v.size();
    state=state*1664525u+1013904223u;v[at]^=static_cast<unsigned char>((state>>24)|1);
    const bool accepted=c::RetargetOpaqueSm89To86(v,out,why);
    if(accepted) {
      Check(out.size()==v.size(),"mutation output size");size_t n=0;
      for(size_t j=0;j<v.size();++j)if(v[j]!=out[j]){++n;Check(v[j]==89 && out[j]==86,"mutation write set");}
      Check(n==3,"mutation cannot expand write set");
    } else Check(out.empty(),"rejection publishes no output");
  }
  // Legacy provider identity and complete production transaction.
  Bytes image(0x4000);IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=128;Put(image,0,dos);
  IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
  nt.FileHeader.NumberOfSections=2;nt.FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
  nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt.OptionalHeader.SizeOfImage=image.size();
  nt.OptionalHeader.SizeOfHeaders=1024;nt.OptionalHeader.NumberOfRvaAndSizes=16;
  nt.OptionalHeader.DataDirectory[0]={0x2000,128};Put(image,128,nt);
  IMAGE_SECTION_HEADER code{},data{};code.VirtualAddress=0x1000;code.Misc.VirtualSize=0x1000;
  code.Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;data.VirtualAddress=0x2000;data.Misc.VirtualSize=0x2000;
  data.Characteristics=IMAGE_SCN_MEM_READ;Put(image,128+sizeof(nt),code);Put(image,128+sizeof(nt)+sizeof(code),data);
  IMAGE_EXPORT_DIRECTORY dir{};dir.Name=0x2060;Put(image,0x2000,dir);
  std::memcpy(image.data()+0x2060,"nvngx_dlssg.dll",15);
  const unsigned char gate[]={0xb8,0x90,1,0,0,0xc3};std::memcpy(image.data()+0x1000,gate,6);
  std::copy(good.begin(),good.end(),image.begin()+0x2200);
  HMODULE module=image.data();provider_mock::regions={{module,image.size()}};
  constexpr const char* exports[]={"NVSDK_NGX_GetGPUArchitecture","NVSDK_NGX_GetSnippetVersion","NVSDK_NGX_D3D12_Init",
   "NVSDK_NGX_D3D12_CreateFeature","NVSDK_NGX_D3D12_EvaluateFeature","NVSDK_NGX_D3D12_GetScratchBufferSize",
   "NVSDK_NGX_D3D12_PopulateParameters_Impl","NVSDK_NGX_D3D12_ReleaseFeature","NVSDK_NGX_D3D12_Shutdown"};
  for(const auto* name:exports)provider_mock::exports[{module,name}]=reinterpret_cast<FARPROC>(image.data()+0x1000);
  Check(p::IsDlssgProvider(module),"pre-requirements legacy provider detected without private kernel markers");
  provider_mock::exports.erase({module,"NVSDK_NGX_D3D12_GetScratchBufferSize"});
  Check(!p::IsDlssgProvider(module),"incomplete common legacy lifecycle rejected");
  provider_mock::exports[{module,"NVSDK_NGX_D3D12_GetScratchBufferSize"}]=reinterpret_cast<FARPROC>(image.data()+0x1000);
  image[0x2060+10]='x';Check(!p::IsDlssgProvider(module),"different export name rejected");
  std::memcpy(image.data()+0x2060,"nvngx_dlssg.dll",15);
  provider_mock::exports.erase({module,"NVSDK_NGX_D3D12_Init"});Check(!p::IsDlssgProvider(module),"incomplete legacy ABI rejected");
  provider_mock::exports[{module,"NVSDK_NGX_D3D12_Init"}]=reinterpret_cast<FARPROC>(image.data()+0x1000);
  mfgunlock::g_enabled=true;mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);
  const auto before=image;Check(p::PrepareProvider(module),"full legacy production prepare");
  Check(p::PreparedProviderCount()==1,"prepared inventory");const auto writes=provider_mock::protection_calls;
  Check(p::PrepareProvider(module)&&writes==provider_mock::protection_calls,"repeated preparation idempotent");
  p::Restore();Check(before==image&&p::internal::g_providers.empty(),"complete rollback");
  provider_mock::protection_calls=0;provider_mock::fail_protection_call=3;
  Check(!p::PrepareProvider(module),"write failure returned");Check(before==image,"failed write rolled back");
  provider_mock::fail_protection_call=0;p::Restore();
  std::cout<<"PASS "<<checks<<" checks; 2048 mutations; no GPU execution\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
