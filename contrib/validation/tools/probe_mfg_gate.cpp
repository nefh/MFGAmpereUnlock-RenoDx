// SPDX-License-Identifier: MIT
// CPU-only production resolver + mock transaction on a private mapped image.
// No LoadLibrary, vendor execution, CUDA, or writes to the input file.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace p=mfgunlock::provider;
namespace mg=mfgunlock::mfggate;
void Text(std::string_view s){std::cout<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')std::cout<<'\\'<<c;else if(c>=32&&c<127)std::cout<<c;else std::cout<<'?';}std::cout<<'"';}
void Hex(std::span<const unsigned char> b){constexpr char h[]="0123456789abcdef";std::cout<<'"';for(auto x:b)std::cout<<h[x>>4]<<h[x&15];std::cout<<'"';}
void Bool(bool b){std::cout<<(b?"true":"false");}
int main(int argc,char**argv){try{
  if(argc!=3)throw std::runtime_error("usage: probe_mfg_gate image.bin Ampere|Turing|Ada");
  const auto size=std::filesystem::file_size(argv[1]);if(size<64||size>128*1024*1024)throw std::runtime_error("mapped image size outside 64B..128MiB");
  std::vector<unsigned char> bytes(static_cast<size_t>(size));std::ifstream in(argv[1],std::ios::binary);
  if(!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))throw std::runtime_error("read failed");
  const std::string architecture=argv[2];
  if(architecture!="Ampere"&&architecture!="Turing"&&architecture!="Ada")throw std::runtime_error("invalid architecture");
  mfgunlock::architecture::Configure(architecture=="Ada"?mfgunlock::Architecture::kAda:architecture=="Turing"?mfgunlock::Architecture::kTuring:mfgunlock::Architecture::kAmpere);
  mfgunlock::g_enabled=true;
  const auto module=reinterpret_cast<HMODULE>(bytes.data());
  provider_mock::regions.push_back({module,bytes.size(),MEM_IMAGE,PAGE_EXECUTE_READ});
  const auto original=bytes;
  p::MfgGatePlan plan;const bool found=p::FindMfgGatePlan(module,plan);
  const bool validated=found&&p::ValidateMfgGatePlan(plan);
  std::vector<p::internal::BytePatch> undo;
  const bool applied=validated&&p::ApplyMfgGatePlan(plan,undo);
  size_t changed=0;for(size_t i=0;i<bytes.size();++i)changed+=bytes[i]!=original[i];
  const bool repeat_rejected=!applied||!p::ApplyMfgGatePlan(plan,undo);
  const bool restored=undo.empty()||p::RestoreMfgComparisons(undo);
  const bool identical=restored&&bytes==original;
  std::cout<<"{\"schema\":\"mfg_gate_probe_mg1\",\"compiled_source_id\":\""<<MFG_GATE_SOURCE_ID<<"\",\"architecture\":";Text(architecture);
  std::cout<<",\"attempted\":true,\"registry_used\":false,\"qualification\":";Text(mg::Name(plan.contract.result));
  std::cout<<",\"reason\":";Text(plan.contract.reason);
  std::cout<<",\"ceiling\":"<<plan.contract.ceiling<<",\"candidates\":"<<plan.contract.candidates<<",\"flag_offset\":"<<plan.contract.flag_offset<<",\"query_slot\":"<<plan.contract.query_slot;
  std::cout<<",\"validated\":";Bool(validated);std::cout<<",\"applied_copy\":";Bool(applied);
  std::cout<<",\"changed_bytes\":"<<changed<<",\"repeat_rejected\":";Bool(repeat_rejected);
  std::cout<<",\"rollback_identical\":";Bool(identical);std::cout<<",\"vendor_code_executed\":false,\"runtime_tested\":false,\"sites\":[";
  for(size_t n=0;found&&n<plan.contract.sites.size();++n){auto s=plan.contract.sites[n];if(n)std::cout<<',';std::cout<<"{\"instruction\":"<<s.instruction<<",\"immediate\":"<<s.immediate<<",\"length\":"<<s.length<<'}';}
  std::cout<<"],\"functions\":[";for(size_t n=0;n<plan.contract.functions.size();++n){if(n)std::cout<<',';std::cout<<plan.contract.functions[n];}
  std::cout<<"],\"witnesses\":[";
  for(size_t n=0;n<plan.contract.witnesses.size();++n){const auto& w=plan.contract.witnesses[n];if(n)std::cout<<',';std::cout<<"{\"rva\":"<<w.rva<<",\"bytes\":"<<w.bytes.size()<<",\"hex\":";Hex(w.bytes);std::cout<<'}';}
  std::cout<<"],\"anchor_windows\":[";
  p::internal::Image image;bool comma=false;size_t emitted=0;
  if(p::internal::InspectImage(module,image)){
    std::vector<mg::Range> ranges;for(const auto& s:image.sections)ranges.push_back({s.VirtualAddress,s.Misc.VirtualSize,(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0});
    mg::View v{bytes,ranges,{}};
    if(v.ReadFunctions())for(auto anchor:{"DLSSG.MultiFrameCountMax","EndpointCore::Create","EndpointCore::Evaluate","EndpointCoreInputs::ComputeAndValidateTimeFactor"})for(const auto& g:mg::Anchored(v,anchor)){
      const size_t length=g.end-g.begin;if(emitted+length>512*1024)continue;emitted+=length;
      if(comma)std::cout<<',';
      comma=true;std::cout<<"{\"anchor\":";Text(anchor);std::cout<<",\"rva\":"<<g.begin<<",\"bytes\":"<<length<<",\"stopped_paths\":"<<g.stopped<<",\"hex\":";Hex(std::span<const unsigned char>(bytes).subspan(g.begin,length));std::cout<<'}';
    }
  }
  std::cout<<"]}\n";
  return !identical||(found&&(!applied||changed!=2))?3:0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
