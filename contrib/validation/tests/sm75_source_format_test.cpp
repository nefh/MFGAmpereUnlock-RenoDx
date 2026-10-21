// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/ptx_retarget.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
namespace p=mfgunlock::ptx;
namespace f=mfgunlock::fatbin;
using Bytes=std::vector<unsigned char>;
unsigned checks=0;
void Check(bool c,const char* why){++checks;if(!c)throw std::runtime_error(why);}
template<class T>void Put(Bytes& b,size_t at,T value){std::memcpy(b.data()+at,&value,sizeof(value));}
Bytes Container(Bytes raw) {
  Bytes payload;payload.push_back(static_cast<unsigned char>(std::min<size_t>(15,raw.size())<<4));
  if(raw.size()>=15){size_t n=raw.size()-15;while(n>=255){payload.push_back(255);n-=255;}payload.push_back(static_cast<unsigned char>(n));}
  payload.insert(payload.end(),raw.begin(),raw.end());
  Bytes b(80+((payload.size()+7)&~size_t{7}));
  Put<uint32_t>(b,0,f::kMagic);Put<uint16_t>(b,4,1);Put<uint16_t>(b,6,16);Put<uint64_t>(b,8,b.size()-16);
  Put<uint16_t>(b,16,1);Put<uint16_t>(b,18,0x101);Put<uint32_t>(b,20,64);Put<uint64_t>(b,24,b.size()-80);
  Put<uint32_t>(b,32,static_cast<uint32_t>(payload.size()));Put<uint32_t>(b,44,89);Put<uint64_t>(b,56,0x2041);Put<uint64_t>(b,72,raw.size());
  std::copy(payload.begin(),payload.end(),b.begin()+80);return b;
}
Bytes Text(std::string s){Bytes b(s.begin(),s.end());b.push_back(0);return b;}
int main(){try{
  const std::string header=".version 99.7\n.target sm_89\n.address_size 64\n";
  const auto turing=mfgunlock::architecture::kTuring;
  const auto ampere=mfgunlock::architecture::kAmpere;
  for(const auto& prefix:{std::string{},std::string(" // note with \" quote\n /* comment */\r\n")}) {
    const auto raw=Text(prefix+header+".file 1 \"C:\\\\shader.cu\"\n.visible .entry kernel(){ ret; }\n");
    Check(p::HasTextPtxPrefix(raw),"unknown ISA number and comments do not authorize/reject by version");
    const auto input=Container(raw);p::Plan plan;std::string reason;
    Check(p::Retarget(input,plan,reason,turing)==p::Result::kRetargeted,reason.c_str());
    Check(p::Retarget(input,plan,reason,ampere)==p::Result::kRetargeted,"Ampere preserved");
  }
  auto malformed=Text(header+".file 1 \"broken\nret;\n");p::Plan plan;std::string reason;
  Check(p::HasTextPtxPrefix(malformed),"recognizable text still reaches real grammar validation");
  auto input=Container(malformed);
  Check(p::Retarget(input,plan,reason,turing)==p::Result::kRejected&&
        reason=="PTX header is not structurally valid"&&plan.replacement.empty(),
        "malformed quoted source still rejected before publication or native fallback");
  // Kind-1 + valid LZ4 does not imply text. The old path interpreted the random
  // 0x22 as a PTX string start and produced the wrong diagnostic.
  Bytes opaque{0x1f,0x3a,0x3d,0xb6,0xbb,0x8d,0x7a,0xf5,0x22,0x41,0x0a,0x33,0};
  const auto opaque_input=Container(opaque);
  Check(!p::HasTextPtxPrefix(opaque),"opaque source classified before grammar");
  Check(p::Retarget(opaque_input,plan,reason,turing)==p::Result::kRejected&&reason.find("opaque/unsupported SM89 source encoding")!=reason.npos,"opaque reason is not a quote error");
  Check(plan.replacement.empty(),"opaque input publishes no replacement");
  std::string old_reason;p::Sm75LoweringCounts counts;std::string old_text(opaque.begin(),opaque.end()-1);
  Check(!p::LowerSm75Structurally(old_text,counts,old_reason)&&old_reason=="SM75 unterminated quoted line","baseline failure reproduced independently");
  std::mt19937 random(0x75f2);
  for(unsigned k=0;k<512;++k){
    Bytes raw(128);for(auto& c:raw)c=static_cast<unsigned char>(random());raw[0]=0x1f;raw[10]='"';raw[20]='\n';raw.back()=0;
    auto blob=Container(raw);const auto before=blob;
    Check(p::Retarget(blob,plan,reason,turing)==p::Result::kRejected&&plan.replacement.empty(),"fuzz-like opaque source rejected");
    Check(blob==before,"classification never edits input");
  }
  std::cout<<"sm75_source_format_test PASS checks="<<checks<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<"\n";return 1;}}
