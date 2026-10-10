// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/ptx_retarget.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ptx = mfgunlock::ptx;
namespace fb = mfgunlock::fatbin;
namespace {
size_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
std::string Program(std::string instruction, bool scalar=false) {
  return ".version 8.2\n.target sm_89\n.address_size 64\n.visible .entry renamed_kernel() {\n" +
      std::string(scalar?".reg .b16 %rs<8>;\n":".reg .b32 %r<8>;\n") + instruction + "\nret;\n}\n";
}
std::string Lower(std::string input, size_t count=1) {
  ptx::Sm75LoweringCounts counts;
  std::string why;
  Check(ptx::LowerSm75Structurally(input,counts,why),why.c_str());
  Check(counts.fma_relu==count,"ReLU count");
  return input;
}
struct Instruction { std::string op; std::vector<std::string> args; };
// Interpret only the emitted post-FMA integer/move instructions. The FMA itself
// is not emulated: its native FP16 operation and operands are checked separately.
std::vector<Instruction> Tail(const std::string& text) {
  std::vector<Instruction> result;
  std::istringstream input(text);
  bool after_fma=false;
  for(std::string line;std::getline(input,line);) {
    for(auto& c:line)if(c==',' || c=='{' || c=='}' || c==';')c=' ';
    std::istringstream parts(line);Instruction item;
    if(!(parts>>item.op))continue;
    if(item.op.starts_with("fma.")){Check(!after_fma,"exactly one fused operation");after_fma=true;continue;}
    if(!after_fma || item.op=="ret")continue;
    for(std::string token;parts>>token;)item.args.push_back(token);
    Check(item.op=="mov.b32" || item.op=="mov.b16" || item.op=="and.b16" ||
          item.op=="setp.gt.u16" || item.op=="selp.b16","unexpected emitted instruction");
    result.push_back(item);
  }
  Check(after_fma,"missing original FMA");
  return result;
}
uint32_t RunTail(const std::vector<Instruction>& tail, uint32_t value, bool scalar) {
  std::map<std::string,uint32_t> regs;
  regs[scalar?"%mfg_relu_lo":"%mfg_relu_value"]=value;
  const auto read=[&](const std::string& s) -> uint32_t {
    if(s.starts_with('%'))return regs.at(s);
    return static_cast<uint32_t>(std::stoul(s,nullptr,0));
  };
  for(const auto& i:tail) {
    const auto& a=i.args;
    if(i.op=="mov.b32" && a[0]=="%mfg_relu_lo") {
      const auto v=read(a[2]);regs[a[0]]=v&0xffffu;regs[a[1]]=v>>16;
    }else if(i.op=="mov.b32") {
      regs[a[0]]=(read(a[1])&0xffffu)|(read(a[2])<<16);
    }else if(i.op=="mov.b16") {
      regs[a[0]]=read(a[1])&0xffffu;
    }else if(i.op=="and.b16") {
      regs[a[0]]=(read(a[1])&read(a[2]))&0xffffu;
    }else if(i.op=="setp.gt.u16") {
      regs[a[0]]=(read(a[1])&0xffffu)>(read(a[2])&0xffffu);
    }else if(i.op=="selp.b16") {
      regs[a[0]]=(read(a[3])?read(a[1]):read(a[2]))&0xffffu;
    }
  }
  return regs.at(scalar?"%rs0":"%r0");
}
uint16_t Expected(uint16_t bits) {
  const unsigned exp=(bits>>10)&31u, fraction=bits&1023u;
  if(exp==31u && fraction)return 0x7fff;  // CUDA canonical half NaN.
  if(bits&0x8000)return 0;               // Clamp including -0 to +0.
  return bits;
}
void NumericTests() {
  const auto packed=Lower(Program("fma.rn.f16x2.relu %r0,%r1,%r2,%r3;"));
  const auto scalar=Lower(Program("fma.rn.relu.f16 %rs0,%rs1,%rs2,%rs3;",true));
  const auto packed_tail=Tail(packed),scalar_tail=Tail(scalar);
  for(uint32_t v=0;v<=0xffff;++v) {
    const uint32_t other=0xffffu-v;
    const uint32_t expected=Expected(static_cast<uint16_t>(v))|
        (static_cast<uint32_t>(Expected(static_cast<uint16_t>(other)))<<16);
    Check(RunTail(packed_tail,v|(other<<16),false)==expected,"packed lane saturation");
    Check(RunTail(scalar_tail,v,true)==Expected(static_cast<uint16_t>(v)),"scalar saturation");
  }
  for(const uint16_t value:{uint16_t{0},uint16_t{0x8000},uint16_t{1},uint16_t{0x8001},
      uint16_t{0x3c00},uint16_t{0x4000},uint16_t{0x7bff},uint16_t{0x7c00},uint16_t{0xfc00},
      uint16_t{0x7c01},uint16_t{0xfc01},uint16_t{0x7fff},uint16_t{0xffff}}) {
    Check(RunTail(packed_tail,value|(0x3c00u<<16),false)==(Expected(value)|(0x3c00u<<16)),"sentinel lane retained");
  }
}
void GrammarTests() {
  const std::string operation="fma.rn.f16x2.relu %r0,%r1,%r2,%r3;";
  const auto source=Program(operation);
  auto result=Lower(source);
  Check(result.find("fma.rn.f16x2 %mfg_relu_value, %r1, %r2, %r3;")!=result.npos,"FMA preserved RN and operands");
  Check(result.find(".relu")==result.npos,"no unlowered ReLU");
  Check(result.find(".ftz")==result.npos,"no FTZ introduced");
  Check(result.find(".sat")==result.npos,"no [0,1] saturation introduced");
  Check(result.find("mul.")==result.npos && result.find("add.")==result.npos,"FMA not split");
  auto again=result;ptx::Sm75LoweringCounts counts;std::string why;
  Check(ptx::LowerSm75Structurally(again,counts,why)&&again==result&&counts.Total()==0,"idempotent composer");
  for(const std::string opcode:{"fma.rn.relu.f16x2","fma.rn.f16x2.relu"}) {
    for(unsigned a=0;a<4;++a)for(unsigned b=0;b<4;++b)for(unsigned c=0;c<4;++c) {
      const std::string operands=" %r0, %r"+std::to_string(a)+",%r"+std::to_string(b)+", %r"+std::to_string(c)+";";
      const auto lowered=Lower(Program(opcode+operands));
      Check(lowered.find("fma.rn.f16x2 %mfg_relu_value, %r"+std::to_string(a)+", %r"+std::to_string(b)+", %r"+std::to_string(c)+";")!=lowered.npos,"aliased source consumed before destination");
    }
  }
  for(const std::string instruction:{"  fma.rn.relu.f16x2\t%r0, %r1 , %r2, %r3; // ignored .relu",
      "{ fma.rn.f16x2.relu %r0,%r1,%r2,%r3; }", "fma.rn.f16x2.relu %r0,/* x */%r1,%r2,%r3;"})
    Lower(Program(instruction));
  for(const std::string instruction:{"fma.rn.relu.f16 %rs0,%rs1,%rs2,%rs3;",
      "{ fma.rn.f16.relu %rs0,%rs1,%rs2,%rs3; }"})Lower(Program(instruction,true));
  const auto comment_only=Program("/* fma.rn.f16x2.relu */\n// .relu\n.pragma \".relu\";\nmov.b32 %r0, 0;");
  Check(Lower(comment_only,0)==comment_only,"comment and string not code");
  for(const std::string invalid:{
      "fma.rz.f16x2.relu %r0,%r1,%r2,%r3;", "fma.rn.ftz.f16x2.relu %r0,%r1,%r2,%r3;",
      "fma.rn.sat.f16x2.relu %r0,%r1,%r2,%r3;", "fma.rn.relu.f16x2.relu %r0,%r1,%r2,%r3;",
      "fma.rn.bf16x2.relu %r0,%r1,%r2,%r3;", "@%p0 fma.rn.f16x2.relu %r0,%r1,%r2,%r3;",
      "@!%p0 fma.rn.f16x2.relu %r0,%r1,%r2,%r3;", "fma.rn.f16x2.relu %r0,%r1,%r2,0;",
      "fma.rn.f16x2.relu %r0,%r1,%r2,%r8;", "fma.rn.f16x2.relu %r00,%r1,%r2,%r3;",
      "fma.rn.f16x2.relu %r0,%r1,%r2;", "fma.rn.f16x2.relu %r0,%r1,%r2,%r3; ret;",
      "fma.rn.f16x2.relu %r0,%r1,%r2,%r3", "fma.rn.f16x2.relu %rs0,%r1,%r2,%r3;",
      "cvt.rn.relu.f16.f32 %r0,%r1;"}) {
    auto input=Program(invalid), saved=input;
    Check(!ptx::LowerSm75Structurally(input,counts,why)&&input==saved&&!why.empty(),"invalid instruction transactional reject");
  }
  for(const std::string suffix:{".reg .b32 %r0;\n", ".reg .b32 %mfg_relu_value;\n",
                               ".visible .entry another() { ret; }\n", ".func helper() { ret; }\n"}) {
    auto input=source+suffix,saved=input;
    Check(!ptx::LowerSm75Structurally(input,counts,why)&&input==saved,"scope collision reject");
  }
  for(unsigned target:{86u,89u}) {
    auto input=source;
    Check(ptx::LowerTuringFmaRelu(input,target,1,why)&&input==source,"other targets unchanged");
  }
  for(size_t count:{size_t{0},size_t{2}}) {
    auto input=source;
    Check(!ptx::LowerTuringFmaRelu(input,75,count,why)&&input==source,"wrong occurrence inventory");
  }
  auto input=source;
  Check(!ptx::LowerTuringFmaRelu(input,70,1,why)&&input==source,"unknown lowering target");
  std::vector<unsigned char> raw(source.begin(),source.end());raw.push_back(0);size_t digit=0;
  Check(!ptx::FindTargetDigits(raw,75,digit),"unlowered modifier denied by final validator");
  Check(ptx::FindTargetDigits(raw,86,digit),"modifier valid for Ampere target");
  raw.assign(result.begin(),result.end());raw.push_back(0);
  Check(ptx::FindTargetDigits(raw,75,digit),"lowered SM75 accepted");
}
void ContainerTests() {
  const auto source=Program("fma.rn.f16x2.relu %r0,%r1,%r2,%r3;");
  std::vector<unsigned char> raw(source.begin(),source.end());raw.push_back(0);
  const auto put=[](auto& bytes,size_t at,auto v) {std::memcpy(bytes.data()+at,&v,sizeof(v));};
  std::vector<unsigned char> payload;
  payload.push_back(0xf0);size_t extra=raw.size()-15;
  while(extra>=255){payload.push_back(255);extra-=255;}
  payload.push_back(static_cast<unsigned char>(extra));payload.insert(payload.end(),raw.begin(),raw.end());
  for(const auto header:{size_t{64},size_t{120}}) {
    std::vector<unsigned char> bin(16+header+((payload.size()+7)&~size_t{7}));
    put(bin,0,fb::kMagic);put(bin,4,uint16_t{1});put(bin,6,uint16_t{16});put(bin,8,uint64_t(bin.size()-16));
    put(bin,16,uint16_t{1});put(bin,18,uint16_t{0x101});put(bin,20,uint32_t(header));
    put(bin,24,uint64_t(bin.size()-16-header));put(bin,32,uint32_t(payload.size()));put(bin,44,uint32_t{89});
    put(bin,56,uint64_t(header==120?0x2011:0x2041));put(bin,72,uint64_t(raw.size()));
    std::copy(payload.begin(),payload.end(),bin.begin()+16+header);
    auto saved=bin;ptx::Plan plan;std::string why;
    Check(ptx::Retarget(bin,plan,why,mfgunlock::architecture::kTuring)==ptx::Result::kRetargeted,why.c_str());
    Check(bin==saved,"input fatbin remains read only");
    std::vector<fb::Entry> entries;size_t end=0;
    Check(fb::Parse(plan.replacement,entries,end)&&entries.size()==1,"rebuilt container valid");
    const auto& e=entries[0];std::vector<unsigned char> output;
    if(e.compressed_bytes) {
      output.resize(e.unpacked_bytes);
      Check(fb::Lz4BlockDecompress(plan.replacement.data()+e.PayloadOffset(),e.compressed_bytes,output.data(),output.size()),"new LZ4 roundtrip");
    }else output.assign(plan.replacement.begin()+e.PayloadOffset(),plan.replacement.begin()+e.End());
    const std::string text(output.begin(),std::find(output.begin(),output.end(),0));
    Check(text.find(".target sm_75")!=text.npos&&text.find(".relu")==text.npos,"rebuilt PTX lowered and retargeted");
  }
}
}
int main() {
  try {
    GrammarTests();ContainerTests();NumericTests();
    std::cout<<"SM75_RELU_PASS checks="<<checks<<" half_encodings=65536 packed_lanes=2 scalar=1 gpu_execution=NOT_RUN\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
}
