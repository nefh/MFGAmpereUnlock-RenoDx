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
Bytes Elf(uint32_t source_sm=89,uint32_t other_flags=0x500) {
  Bytes b(512);
  std::memcpy(b.data(),"\177ELF\2\1\1\63\7",9);
  Put<uint16_t>(b,16,2);Put<uint16_t>(b,18,190);Put<uint32_t>(b,20,118);
  Put<uint64_t>(b,40,256);Put<uint32_t>(b,48,source_sm|(source_sm<<16)|(other_flags&0xff00ff00u));
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
Bytes Fatbin(const Bytes& elf,bool plaintext=false,uint32_t source_sm=89) {
  Bytes raw(128,0x91);
  if(plaintext) {
    const std::string text=".version 8.7\n.target sm_"+std::to_string(source_sm)+
        "\n.address_size 64\n.visible .entry test() { ret; }\n";
    raw.assign(text.begin(),text.end());raw.push_back(0);
  }
  Bytes compressed;
  const std::string_view view(reinterpret_cast<const char*>(raw.data()),raw.size());
  Check(f::Lz4BlockCompress(view,1024,compressed),"fixture compression");
  const size_t padded=(compressed.size()+7)&~size_t{7};
  Bytes b(16+64+padded+64+elf.size());
  Put<uint32_t>(b,0,f::kMagic);Put<uint16_t>(b,4,1);Put<uint16_t>(b,6,16);Put<uint64_t>(b,8,b.size()-16);
  Put<uint16_t>(b,16,1);Put<uint16_t>(b,18,0x101);Put<uint32_t>(b,20,64);Put<uint64_t>(b,24,padded);
  Put<uint32_t>(b,32,static_cast<uint32_t>(compressed.size()));Put<uint32_t>(b,44,source_sm);
  Put<uint64_t>(b,56,0x2041);Put<uint64_t>(b,64,0x98765432);Put<uint64_t>(b,72,raw.size());
  std::copy(compressed.begin(),compressed.end(),b.begin()+80);
  const auto e=80+padded;
  Put<uint16_t>(b,e,2);Put<uint16_t>(b,e+2,0x101);Put<uint32_t>(b,e+4,64);Put<uint64_t>(b,e+8,elf.size());
  Put<uint32_t>(b,e+28,source_sm);Put<uint64_t>(b,e+40,0x41);std::copy(elf.begin(),elf.end(),b.begin()+e+64);
  return b;
}
void AppendNative(Bytes& to,const Bytes& source) {
  std::vector<f::Entry> entries;size_t end=0;
  Check(f::Parse(source,entries,end),"append fixture source bounds");
  const auto found=std::find_if(entries.begin(),entries.end(),[](const auto& entry){return entry.kind==2;});
  Check(found!=entries.end(),"append fixture has native image");
  to.insert(to.end(),source.begin()+found->offset,source.begin()+found->End());
  Put<uint64_t>(to,8,to.size()-16);
}
Bytes NativeOnly(const Bytes& source) {
  Bytes out(source.begin(),source.begin()+16);AppendNative(out,source);return out;
}
void GenericSources() {
  std::string why;Bytes out;
  for(uint32_t source:{90u,120u,255u}) {
    const auto elf=Elf(source,0xa3000500u);
    uint32_t observed=0;
    Check(c::InspectNative(elf,why,observed)&&observed==source,
          "ELF inspector resolves actual unlisted source metadata");
    const auto input=Fatbin(elf,false,source);
    std::vector<f::Entry> entries;size_t end=0;
    Check(f::Parse(input,entries,end),"generic source fixture bounds");
    const auto& native=entries.back();
    for(uint32_t target:{75u,86u}) {
      observed=999;
      Check(c::RetargetNativeToTarget(input,out,why,target,&observed)&&observed==source,
            "generic source reaches a real bounded native metadata trial");
      std::vector<size_t> differences;
      for(size_t i=0;i<input.size();++i)if(input[i]!=out[i])differences.push_back(i);
      Check(differences==std::vector<size_t>{native.offset+28,native.PayloadOffset()+48,native.PayloadOffset()+50},
            "generic trial changes exactly the three architecture fields and preserves all SASS/other bytes");
      Check(f::ReadU32(out.data()+native.offset+28)==target &&
            f::ReadU32(out.data()+native.PayloadOffset()+48)==(target|(target<<16)|0xa3000500u),
            "other ELF flag bits and the full fatbin architecture field are preserved");
      const auto targeted=out;
      Check(!c::RetargetNativeToTarget(targeted,out,why,target,&observed)&&out.empty()&&observed==0&&
            why.find("already present")!=std::string::npos,
            "existing native target is a no-op, never fake retarget success");
      const auto only=NativeOnly(input);
      Check(c::RetargetNativeToTarget(only,out,why,target,&observed)&&observed==source,
            "generic native-only source gets the same empirical trial");
      Check(!c::RetargetNativeToTarget(Fatbin(elf,true,source),out,why,target)&&out.empty(),
            "plaintext paired source never enters the opaque native fallback");
    }
  }
  const auto sm90=Fatbin(Elf(90),false,90),sm120=Fatbin(Elf(120),false,120);
  auto alternatives=NativeOnly(sm120);
  AppendNative(alternatives,sm120);  // An unselected duplicate must not confuse the nearer source.
  const size_t nearer=alternatives.size();AppendNative(alternatives,sm90);
  uint32_t observed=0;
  Check(c::RetargetNativeToTarget(alternatives,out,why,86,&observed)&&observed==90,
        "unique closest higher native source wins independently of entry order");
  Check(std::equal(alternatives.begin(),alternatives.begin()+nearer,out.begin()),
        "unselected higher native siblings remain byte exact");
  auto lower=NativeOnly(Fatbin(Elf(75),false,75));
  const size_t higher_at=lower.size();AppendNative(lower,sm90);
  Check(c::RetargetNativeToTarget(lower,out,why,86,&observed)&&observed==90&&
        std::equal(lower.begin(),lower.begin()+higher_at,out.begin()),
        "a lower native sibling does not veto the unique closest higher-source trial");
  lower=Fatbin(Elf(75),false,75);  // Includes a lower PTX sibling as well.
  AppendNative(lower,sm120);
  Check(c::RetargetNativeToTarget(lower,out,why,86,&observed)&&observed==120,
        "unselected lower PTX and native siblings remain independent of source selection");
  AppendNative(alternatives,sm90);
  Check(!c::RetargetNativeToTarget(alternatives,out,why,86)&&out.empty(),
        "two equally close native sources are an actual selection ambiguity");
  alternatives=NativeOnly(sm90);
  std::vector<f::Entry> entries;size_t end=0;
  Check(f::Parse(alternatives,entries,end),"metadata conflict fixture bounds");
  const auto native=entries[0];
  for(size_t field:{native.PayloadOffset()+48,native.PayloadOffset()+50}) {
    auto broken=alternatives;broken[field]=120;
    Check(!c::RetargetNativeToTarget(broken,out,why,86)&&out.empty(),
          "distinct ELF SM fields are not silently rewritten by the matching-field recipe");
    Check(why.find("outside this metadata-trial recipe")!=std::string::npos,
          "distinct SM values are reported as recipe coverage, not universally invalid ELF");
  }
  for(uint32_t source:{120u,256u,999u}) {
    auto broken=alternatives;Put<uint32_t>(broken,native.offset+28,source);
    Check(!c::RetargetNativeToTarget(broken,out,why,86)&&out.empty(),
          "fatbin/ELF mismatch and wider unrepresentable source values are never truncated");
  }
  auto broken=alternatives;broken[native.PayloadOffset()+4]=1;AppendNative(broken,sm120);
  Check(!c::RetargetNativeToTarget(broken,out,why,86)&&out.empty(),
        "malformed closest native source is not silently skipped for a different payload");
  auto exact=NativeOnly(Fatbin(Elf(86),false,86));AppendNative(exact,sm90);AppendNative(exact,sm120);
  Check(!c::RetargetNativeToTarget(exact,out,why,86)&&out.empty()&&why.find("already present")!=std::string::npos,
        "existing exact native target takes priority over all higher alternatives");
}
int main() {
 try {
  GenericSources();
  std::string why;Bytes out;
  const auto elf=Elf();Check(c::InspectSm89(elf,why),"valid ELF layout");
  const auto good=Fatbin(elf);
  Check(c::RetargetOpaqueSm89To86(good,out,why),"native trial");
  std::vector<size_t> changes;for(size_t i=0;i<good.size();++i)if(good[i]!=out[i])changes.push_back(i);
  Check(changes.size()==3,"exactly three metadata bytes");
  for(size_t i:changes)Check(good[i]==89 && out[i]==86,"only target digits");
  std::vector<f::Entry> input_entries; size_t input_end = 0;
  Check(f::Parse(good,input_entries,input_end),"native fixture parsed");
  const auto& native = input_entries.back();
  for (const uint32_t target : {75u,86u}) {
    Check(c::RetargetOpaqueSm89ToTarget(good,out,why,target),"native trial for both existing backport targets");
    Check(f::ReadU32(out.data()+native.offset+28)==target &&
          out[native.PayloadOffset()+48]==target && out[native.PayloadOffset()+50]==target,
          "actual native target metadata");
    Check(std::equal(good.begin()+native.PayloadOffset()+128,good.begin()+native.PayloadOffset()+160,
                     out.begin()+native.PayloadOffset()+128),"native instruction bytes unchanged");
    Bytes only(good.begin(),good.begin()+16);
    only.insert(only.end(),good.begin()+native.offset,good.begin()+native.End());
    Put<uint64_t>(only,8,only.size()-16);
    Check(c::RetargetOpaqueSm89ToTarget(only,out,why,target),"native-only container gets same actual attempt");
  }
  Check(c::RetargetOpaqueSm89To86(good,out,why),"restore reference test target");
  const auto already_retargeted = out;
  Check(!c::RetargetOpaqueSm89To86(already_retargeted,out,why),"already native target not rewritten");
  Check(!c::RetargetOpaqueSm89To86(Fatbin(elf,true),out,why),"no fallback for plaintext PTX");
  auto bad=good;Put<uint64_t>(bad,64,0);Check(c::RetargetOpaqueSm89To86(bad,out,why),"opaque payload decoding does not require an opaque-tag allowlist");
  auto future_abi=elf;future_abi[8]=91;
  Check(c::RetargetOpaqueSm89ToTarget(Fatbin(future_abi),out,why,75),
        "unfamiliar CUDA ABI metadata still receives bounded native trial");
  auto siblings=good;
  const auto source_entry = input_entries.front();
  const size_t sibling_at = siblings.size();
  siblings.insert(siblings.end(), good.begin()+source_entry.offset, good.begin()+source_entry.End());
  Put<uint32_t>(siblings,sibling_at+28,120);Put<uint64_t>(siblings,8,siblings.size()-16);
  Check(c::RetargetOpaqueSm89ToTarget(siblings,out,why,75),"unrelated higher-target sibling does not veto unique native source");
  Check(std::equal(siblings.begin()+sibling_at,siblings.end(),out.begin()+sibling_at),
        "unrelated sibling remains byte exact");
  Put<uint32_t>(siblings,sibling_at+28,75);
  Check(!c::RetargetOpaqueSm89ToTarget(siblings,out,why,75)&&out.empty(),
        "competing eligible target remains an actual selection ambiguity");
  for(size_t n=0;n<elf.size();++n) {
    const bool accepted=c::InspectSm89(std::span(elf).first(n),why);
    if(n<448)Check(!accepted,"truncated tables rejected");
  }
  for(const size_t at:{size_t{4},size_t{5},size_t{7},size_t{16},size_t{18},size_t{48},size_t{50},size_t{52},size_t{58},size_t{62}}) {
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
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kTuring);
  Check(p::PrepareProvider(module),"SM75 full production native trial without architecture refusal");
  Check(image[0x2200+native.offset+28]==75 &&
        image[0x2200+native.PayloadOffset()+48]==75 &&
        std::equal(before.begin()+0x2200+native.PayloadOffset()+128,
                   before.begin()+0x2200+native.PayloadOffset()+160,
                   image.begin()+0x2200+native.PayloadOffset()+128),
        "SM75 native selection and instruction byte preservation");
  p::Restore(); Check(before==image,"SM75 production trial restores full image");
  for(uint32_t source:{90u,120u}) {
    const auto actual=Fatbin(Elf(source,0xa3000500u),false,source);
    Check(actual.size()==good.size(),"generic provider container has identical owned span");
    std::copy(actual.begin(),actual.end(),image.begin()+0x2200);
    const auto source_before=image;
    for(auto architecture:{mfgunlock::Architecture::kTuring,mfgunlock::Architecture::kAmpere}) {
      mfgunlock::architecture::Configure(architecture);
      const uint32_t target=architecture==mfgunlock::Architecture::kTuring?75:86;
      reshade::log::lines.clear();
      Check(p::PrepareProvider(module),"generic source reaches complete production native transaction");
      std::vector<size_t> differences;
      for(size_t i=0;i<actual.size();++i)
        if(image[0x2200+i]!=source_before[0x2200+i])differences.push_back(i);
      Check(differences==std::vector<size_t>{native.offset+28,native.PayloadOffset()+48,native.PayloadOffset()+50},
            "production source90/120 trial preserves all bytes outside three architecture fields");
      Check(image[0x2200+native.offset+28]==target&&
            f::ReadU32(image.data()+0x2200+native.PayloadOffset()+48)==(target|(target<<16)|0xa3000500u),
            "production trial changes selected target and preserves other ELF flags");
      const std::string evidence="native_code_trial source_sm="+std::to_string(source)+
          " target_sm="+std::to_string(target)+" containers=1 instruction_bytes_changed=0";
      Check(std::any_of(reshade::log::lines.begin(),reshade::log::lines.end(),[&](const auto& line){
            return line.find(evidence)!=std::string::npos;
          }),"production log reports actual source/target and zero instruction edits");
      p::Restore();Check(image==source_before&&p::internal::g_providers.empty(),
                        "generic source production rollback restores the entire image");
      provider_mock::fail_protection_call=provider_mock::protection_calls+3;
      Check(!p::PrepareProvider(module)&&image==source_before,
            "generic source production write failure rolls back byte exactly");
      provider_mock::fail_protection_call=0;p::Restore();
    }
  }
  auto already_native=NativeOnly(Fatbin(Elf(86),false,86));
  AppendNative(already_native,Fatbin(Elf(90),false,90));
  std::fill(image.begin()+0x2200,image.end(),0);
  std::copy(already_native.begin(),already_native.end(),image.begin()+0x2200);
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);
  const auto native_before=image;
  const auto native_writes=provider_mock::protection_calls;
  Check(!p::PrepareProvider(module)&&image==native_before&&provider_mock::protection_calls==native_writes&&
        p::PreparedProviderCount()==0,"existing target reaches production no-op without fake retarget commit");
  Check(std::any_of(p::internal::g_rejected.begin(),p::internal::g_rejected.end(),[](const auto& item){
        return item.reason.find("native/target payload already present")!=std::string::npos;
      }),"production no-op explains that no retarget transaction is needed");
  p::Restore();
  std::copy(before.begin(),before.end(),image.begin());
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);
  provider_mock::protection_calls=0;provider_mock::fail_protection_call=3;
  Check(!p::PrepareProvider(module),"write failure returned");Check(before==image,"failed write rolled back");
  provider_mock::fail_protection_call=0;p::Restore();
  std::cout<<"PASS "<<checks<<" checks; 2048 mutations; no GPU execution\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
