// SPDX-License-Identifier: MIT
#pragma once

// A second, independently qualified private-reader ABI: a cached DWORD lookup
// returning a canonical bool in AL, not in the unused high bits of RAX.
// The reader is interpreted over its whole reachable graph. No DLL version,
// whole-image hash or hard-coded function RVA is an admission criterion.
#include "./mfg_gate.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>
#include <span>

namespace mfgunlock::fgpreset::cached {
using mfggate::View;
using mfggate::Instruction;
using mfggate::Graph;

struct Evidence {
  uint32_t accessor = 0;
  std::vector<std::pair<uint32_t, uint32_t>> functions;
};

// Reviewed MSVC static-local cache accessor shape. All instructions, frame
// operations and local branches are matched; ONLY position-dependent rel32
// operands may vary. Repeated references must agree on the SAME object/guard.
// This is a bounded native-code ABI schema, not a version/profile whitelist.
// Its initialization helpers remain native and are never called by the probe.
inline constexpr unsigned char kLocalCacheAccessor[] = {
  0x48,0x83,0xec,0x28,0x8b,0x0d,0x00,0x00,0x00,0x00,0x65,0x48,0x8b,0x04,0x25,0x58,
  0x00,0x00,0x00,0xba,0x04,0x00,0x00,0x00,0x48,0x8b,0x0c,0xc8,0x8b,0x04,0x0a,0x39,
  0x05,0x00,0x00,0x00,0x00,0x7f,0x0c,0x48,0x8d,0x05,0x00,0x00,0x00,0x00,0x48,0x83,
  0xc4,0x28,0xc3,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,0x00,0x83,
  0x3d,0x00,0x00,0x00,0x00,0xff,0x75,0xdf,0xc6,0x05,0x00,0x00,0x00,0x00,0x00,0x48,
  0x8d,0x05,0x00,0x00,0x00,0x00,0x48,0x89,0x44,0x24,0x30,0x0f,0x57,0xc0,0xf3,0x0f,
  0x11,0x05,0x00,0x00,0x00,0x00,0x48,0xc7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x48,0xc7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xb9,0x18,0x00,0x00,
  0x00,0xe8,0x00,0x00,0x00,0x00,0x48,0x89,0x00,0x48,0x89,0x40,0x08,0x48,0x89,0x05,
  0x00,0x00,0x00,0x00,0x0f,0x57,0xc0,0x66,0x0f,0x7f,0x05,0x00,0x00,0x00,0x00,0x48,
  0xc7,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x48,0xc7,0x05,0x00,0x00,0x00,
  0x00,0x07,0x00,0x00,0x00,0x48,0xc7,0x05,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
  0xf3,0x0f,0x10,0x05,0x00,0x00,0x00,0x00,0xf3,0x0f,0x11,0x05,0x00,0x00,0x00,0x00,
  0x4c,0x8b,0xc0,0xba,0x10,0x00,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,
  0x00,0x00,0x00,0x00,0x90,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,
  0x00,0x90,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,0x00,0x48,0x8d,
  0x05,0x00,0x00,0x00,0x00,0x48,0x83,0xc4,0x28,0xc3,
};
struct Relocation { uint32_t instruction, field, next; };
inline constexpr Relocation kAccessorRelocations[] = {
  {0x4, 0x6, 0xa},
  {0x1f, 0x21, 0x25},
  {0x27, 0x2a, 0x2e},
  {0x33, 0x36, 0x3a},
  {0x3a, 0x3b, 0x3f},
  {0x3f, 0x41, 0x46},
  {0x48, 0x4a, 0x4f},
  {0x4f, 0x52, 0x56},
  {0x5e, 0x62, 0x66},
  {0x66, 0x69, 0x71},
  {0x71, 0x74, 0x7c},
  {0x81, 0x82, 0x86},
  {0x8d, 0x90, 0x94},
  {0x97, 0x9b, 0x9f},
  {0x9f, 0xa2, 0xaa},
  {0xaa, 0xad, 0xb5},
  {0xb5, 0xb8, 0xc0},
  {0xc0, 0xc4, 0xc8},
  {0xc8, 0xcc, 0xd0},
  {0xd8, 0xdb, 0xdf},
  {0xdf, 0xe0, 0xe4},
  {0xe5, 0xe8, 0xec},
  {0xec, 0xed, 0xf1},
  {0xf2, 0xf5, 0xf9},
  {0xf9, 0xfa, 0xfe},
  {0xfe, 0x101, 0x105},
};

inline bool QualifiedAccessor(const View& view, uint32_t target, Evidence& evidence) {
  const auto fn = view.Function(target);
  if (fn.first != target || fn.second <= target || !view.InSection(target, fn.second - target, true)) return false;
  Graph graph;
  if (!graph.Build(view, fn) || graph.stopped) return false;
  // Already initialized singleton: LEA RAX,[RIP+object]; RET. No inputs.
  const auto* first = graph.At(target);
  const auto* last = graph.Next(first);
  if (graph.instructions.size() == 2 && first && first->op == 0x8d && first->width == 64 &&
      first->reg == 0 && first->rip && view.InSection(first->target, 0x48, false) &&
      last && last->op == 0xc3 && last->next == fn.second) {
    evidence.accessor = target; evidence.functions.push_back(fn); return true;
  }
  if (fn.second - target != sizeof(kLocalCacheAccessor)) return false;
  std::array<bool, sizeof(kLocalCacheAccessor)> relocation_bytes{};
  for (const auto& relocation : kAccessorRelocations) {
    const auto* instruction = graph.At(target + relocation.instruction);
    if (!instruction || instruction->next != target + relocation.next ||
        (!instruction->rip && !(instruction->call && instruction->op == 0xe8))) return false;
    for (unsigned byte = 0; byte < 4; ++byte) relocation_bytes[relocation.field + byte] = true;
  }
  for (size_t i = 0; i < sizeof(kLocalCacheAccessor); ++i)
    if (!relocation_bytes[i] && view.image[target + i] != kLocalCacheAccessor[i]) return false;
  const auto address = [&](uint32_t offset) { return graph.At(target + offset)->target; };
  const uint32_t object = address(0x27);
  if (!view.InSection(object, 0x4c, false)) return false;
  // Object base, unordered-map storage, mask and static initialization guard.
  for (const auto& [at, offset] : std::initializer_list<std::pair<uint32_t, uint32_t>>{
      {0x1f,0x48},{0x27,0},{0x33,0x48},{0x3f,0x48},{0x48,0},{0x4f,8},
      {0x5e,8},{0x66,0x10},{0x71,0x18},{0x8d,0x10},{0x97,0x20},
      {0x9f,0x30},{0xaa,0x38},{0xb5,0x40},{0xc8,8},{0xd8,0x20},{0xf2,0x48},{0xfe,0}})
    if (address(at) != uint64_t{object} + offset) return false;
  if (!view.InSection(address(0x04), 4, false) || !view.InSection(address(0xc0), 4, false)) return false;
  // The registered destructor can be a leaf tail-call thunk without .pdata:
  // LEA RCX,[RIP+map]; JMP native map destructor. Verify both instructions.
  const uint32_t callback = address(0xe5);
  const auto extent = mfggate::CallableExtent(view, callback);
  if (extent.first != callback || extent.second <= callback) return false;
  const auto object_argument = mfggate::Decode(view, callback, extent.second);
  const auto tail = mfggate::Decode(view, object_argument.next, extent.second);
  const auto destructor = view.Function(tail.target);
  if (object_argument.op != 0x8d || object_argument.width != 64 || object_argument.reg != 1 ||
      !object_argument.rip || object_argument.target != uint64_t{object} + 8 ||
      tail.op != 0xe9 || !tail.jump || destructor.first != tail.target ||
      destructor.second <= destructor.first) return false;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies{fn, {callback, tail.next}, destructor};
  for (const auto& relocation : kAccessorRelocations) {
    const auto& instruction = *graph.At(target + relocation.instruction);
    if (!instruction.call) continue;
    const auto callee = view.Function(instruction.target);
    if (callee.first != instruction.target || callee.second <= callee.first ||
        callee.second - callee.first > mfggate::kMaxFunctionBytes ||
        !view.InSection(callee.first, callee.second - callee.first, true)) return false;
    dependencies.push_back(callee);
  }
  evidence.accessor = target;
  evidence.functions.insert(evidence.functions.end(), dependencies.begin(), dependencies.end());
  return true;
}

// Native-owned addresses are read-only to this interpreter. Only exactly one
// DWORD through the original output pointer, or initialized local stack slots,
// may be written. Original nonvolatile registers must be restored at every RET.
enum class Kind : uint8_t { kUnknown, kWord, kOutput, kNative, kConstant, kBoolean, kSaved };
struct Value {
  Kind kind = Kind::kUnknown;
  unsigned bits = 0;
  uint64_t number = 0;
  bool operator==(const Value&) const = default;
};
inline bool Boolean(const Value& value) {
  return value.kind == Kind::kBoolean || (value.kind == Kind::kConstant && value.number <= 1);
}
inline Value Meet(Value a, Value b) {
  if (a == b) return a;
  const auto bits = (std::min)(a.bits, b.bits);
  if (!bits) return {};
  if (a.kind == Kind::kOutput && b.kind == Kind::kOutput) return {Kind::kOutput,bits,0};
  if (a.kind == Kind::kNative && (b.kind == Kind::kNative || (b.kind == Kind::kConstant && b.number == 0)))
    return {Kind::kNative,bits,0};
  if (b.kind == Kind::kNative && a.kind == Kind::kConstant && a.number == 0) return {Kind::kNative,bits,0};
  if (a.kind == Kind::kSaved || b.kind == Kind::kSaved) return {};
  if (Boolean(a) && Boolean(b)) return {Kind::kBoolean,bits,0};
  return {Kind::kWord,bits,0};
}
inline bool Nonvolatile(unsigned reg) { return reg == 3 || reg == 5 || reg == 6 || reg == 7 || reg >= 12; }
struct Slot { Value value; unsigned bytes = 0; bool operator==(const Slot&) const = default; };
struct State {
  std::array<Value,16> registers{};
  std::map<int64_t,Slot> stack;
  int64_t sp = 0;
  bool flags = false, output = false, called = false;
  bool operator==(const State&) const = default;
};
inline bool MeetState(State& into, const State& other) {
  State merged = into;
  for (size_t i=0; i<16; ++i) merged.registers[i] = Meet(into.registers[i], other.registers[i]);
  for (auto it=merged.stack.begin(); it!=merged.stack.end();) {
    const auto found=other.stack.find(it->first);
    if (found==other.stack.end() || found->second.bytes!=it->second.bytes) it=merged.stack.erase(it);
    else { it->second.value=Meet(it->second.value,found->second.value); ++it; }
  }
  merged.flags=into.flags&&other.flags;
  merged.output=into.output&&other.output;
  merged.called=into.called&&other.called;
  if (merged==into) return false;
  into=std::move(merged);return true;
}
inline bool Prefixes(const View& view, const Instruction& i) {
  size_t at=i.rva;
  if (view.image[at]>=0x40 && view.image[at]<=0x4f) ++at;
  if (view.image[at]==0x66 && i.op==0x0f && i.op2==0x1f) ++at;
  return at<i.next && view.image[at]==i.op;
}
inline bool Qualify(const View& view, uint32_t target, Evidence& evidence) {
  evidence = {};
  const auto fn=view.Function(target);
  Graph graph;
  if (fn.first!=target || !graph.Build(view,fn) || graph.stopped || graph.instructions.empty()) return false;
  std::vector<Instruction> calls;
  for (const auto& [at,i]:graph.instructions) { (void)at; if(i.call)calls.push_back(i); }
  if(calls.size()!=1 || calls[0].op!=0xe8 || !QualifiedAccessor(view,calls[0].target,evidence)) return false;
  State entry;
  entry.registers[1]={Kind::kWord,32,0};entry.registers[2]={Kind::kOutput,64,0};
  for(unsigned reg=0;reg<16;++reg) if(Nonvolatile(reg))entry.registers[reg]={Kind::kSaved,64,reg};
  std::map<uint32_t,State> states{{target,entry}};
  std::vector<uint32_t> pending{target};
  size_t budget=graph.instructions.size()*128+1;
  bool returned=false,success=false;
  while(!pending.empty()) {
    if(!budget--)return false;
    const auto at=pending.back();pending.pop_back();
    const auto* instruction=graph.At(at);
    if(!instruction || !Prefixes(view,*instruction))return false;
    const auto& i=*instruction;
    State s=states.at(at);
    const auto read=[&](unsigned reg,unsigned bits)->Value {
      if(reg>=16 || s.registers[reg].bits<bits || s.registers[reg].kind==Kind::kSaved)return {};
      return s.registers[reg];
    };
    const auto put=[&](unsigned reg,Value value,unsigned bits)->bool {
      if(reg>=16 || reg==4 || !value.bits || (bits!=8 && bits!=32 && bits!=64))return false;
      if(bits==8) {value.bits=8; if(value.kind==Kind::kConstant)value.number&=255;}
      else {
        if(bits==32) {
          if(value.kind==Kind::kOutput || value.kind==Kind::kSaved)value.kind=Kind::kWord;
          if(value.kind==Kind::kConstant)value.number&=UINT32_MAX;
        }
        value.bits=64;
      }
      s.registers[reg]=value;return true;
    };
    const auto load_stack=[&](int64_t offset,unsigned bits)->Value {
      for(const auto& [start,slot]:s.stack) if(offset>=start && uint64_t(offset-start)+bits/8<=slot.bytes) {
        auto v=slot.value;
        if(offset!=start || bits!=slot.bytes*8) {
          if(v.kind==Kind::kSaved || v.kind==Kind::kOutput)return {};
          if(v.kind==Kind::kConstant)v.number>>=8*static_cast<unsigned>(offset-start);
          else v.kind=Kind::kWord;
        }
        v.bits=bits;return v;
      }
      return {};
    };
    const auto store_stack=[&](int64_t offset,unsigned bits,Value v)->bool {
      if(!v.bits || bits%8 || !bits || bits>64 || offset < s.sp || offset< -4096 ||
          offset+bits/8>40 || (offset<8 && offset+bits/8>0))return false;
      for(auto it=s.stack.begin();it!=s.stack.end();) {
        if(it->first<offset+bits/8 && offset<it->first+it->second.bytes)it=s.stack.erase(it);else ++it;
      }
      s.stack[offset]={v,bits/8};return true;
    };
    const auto memory=[&](unsigned bits)->Value {
      if(!i.memory || i.rip || i.index!=16)return {};
      if(i.rm==4)return load_stack(s.sp+i.displacement,bits);
      const auto base=read(i.rm,64);
      if(base.kind==Kind::kNative)return {Kind::kNative,bits,0};
      return {};
    };
    const auto operand=[&](unsigned bits)->Value {return i.memory?memory(bits):read(i.rm,bits);};
    if(i.call) {
      if(at!=calls[0].rva || s.called || ((s.sp+8)&15)!=0 || s.sp> -32)return false;
      for(unsigned reg:{0u,1u,2u,8u,9u,10u,11u})s.registers[reg]={};
      // A callee may use its full 32-byte home area; do not keep aliases there.
      for(auto it=s.stack.begin();it!=s.stack.end();) {
        if(it->first<s.sp+32 && s.sp<it->first+it->second.bytes)it=s.stack.erase(it);else ++it;
      }
      s.registers[0]={Kind::kNative,64,0};s.flags=false;s.called=true;
    } else if(i.terminal) {
      if(i.op!=0xc3 || s.sp!=0 || !s.called || !Boolean(read(0,8)))return false;
      for(unsigned reg=0;reg<16;++reg) if(Nonvolatile(reg) && s.registers[reg]!=entry.registers[reg])return false;
      const bool yes=s.registers[0].kind==Kind::kBoolean || s.registers[0].number==1;
      if(yes&&!s.output)return false;
      returned=true;success|=yes;continue;
    } else if(i.jump) {
      if(i.conditional&&!s.flags)return false;
    } else if((i.op==0x90&&!(i.rex&1)) || (i.op==0x0f&&i.op2==0x1f&&(i.reg&7)==0)) {
      // Architectural NOP, including the operand-size-prefixed form.
    } else if(i.op>=0x50 && i.op<=0x57) {
      const unsigned reg=i.op-0x50+((i.rex&1)?8:0);
      if(reg==4 || s.registers[reg].bits<64)return false;
      s.sp-=8;if(!store_stack(s.sp,64,s.registers[reg]))return false;
    } else if(i.op>=0x58 && i.op<=0x5f) {
      const unsigned reg=i.op-0x58+((i.rex&1)?8:0);
      auto v=load_stack(s.sp,64);if(!put(reg,v,64))return false;s.stack.erase(s.sp);s.sp+=8;
    } else if(!i.memory && i.width==64 && i.rm==4 && (i.op==0x81||i.op==0x83) && ((i.reg&7)==0||(i.reg&7)==5)) {
      const int64_t delta=i.op==0x83?static_cast<int8_t>(i.immediate):static_cast<int32_t>(i.immediate);
      s.sp+=((i.reg&7)==0?delta:-delta);if(s.sp< -4096 || s.sp>0)return false;
      s.flags=true;
    } else if(i.op>=0xb8 && i.op<=0xbf) {
      if(!put(i.reg,{Kind::kConstant,64,i.immediate},i.width))return false;
    } else if(i.op==0xb0 && !(i.rex&1) && i.immediate<=1) {
      if(!put(0,{Kind::kConstant,8,i.immediate},8))return false;
    } else if((i.op==0x8b||i.op==0x89) && !i.memory) {
      const auto source=i.op==0x89?i.reg:i.rm, dest=i.op==0x89?i.rm:i.reg;
      if(!put(dest,read(source,i.width),i.width))return false;
    } else if(i.op==0x89 && i.memory) {
      if(i.rip || i.index!=16)return false;
      if(i.rm==4) {
        if(s.registers[i.reg].bits<i.width || !store_stack(s.sp+i.displacement,i.width,s.registers[i.reg]))return false;
      } else {
        const auto base=read(i.rm,64);
        if(base.kind!=Kind::kOutput || i.width!=32 || i.displacement!=0 || !read(i.reg,32).bits)return false;
        s.output=true;
      }
    } else if(i.op==0x8b && i.memory) {
      const auto v=memory(i.width);
      if(!put(i.reg,v,i.width))return false;
    } else if(i.op==0x0f && i.op2==0xb6) {
      if(!i.memory&&!i.rex&&i.rm>=4&&i.rm<8)return false;
      auto v=operand(8);if(!v.bits || v.kind==Kind::kSaved || v.kind==Kind::kOutput)return false;
      if(v.kind==Kind::kConstant)v.number&=255;else v.kind=Kind::kWord;
      if(!put(i.reg,v,i.width))return false;
    } else if((i.op==0x31||i.op==0x33||i.op==0x30||i.op==0x32) && !i.memory) {
      const auto dest=(i.op==0x31||i.op==0x30)?i.rm:i.reg;
      const auto src=(i.op==0x31||i.op==0x30)?i.reg:i.rm;
      if(i.width==8&&!i.rex&&(dest>=4||src>=4))return false;
      Value v{Kind::kWord,64,0};
      if(src==dest)v={Kind::kConstant,64,0};
      else {if(!read(dest,i.width).bits||!read(src,i.width).bits)return false;
        if(read(dest,i.width).kind==Kind::kNative||read(src,i.width).kind==Kind::kNative)v.kind=Kind::kNative;}
      if(!put(dest,v,i.width))return false;
      s.flags=true;
    } else if((i.op==0x03||i.op==0x23||i.op==0x0b||i.op==0x2b||(i.op==0x0f&&i.op2==0xaf))) {
      const auto a=read(i.reg,i.width),b=operand(i.width);
      if(!a.bits||!b.bits||a.kind==Kind::kOutput||b.kind==Kind::kOutput)return false;
      Value value{a.kind==Kind::kNative||b.kind==Kind::kNative?Kind::kNative:Kind::kWord,64,0};
      if(!put(i.reg,value,i.width))return false;
      s.flags=true;
    } else if(i.op==0xc1&&!i.memory&&i.width==64&&((i.reg&7)==4||(i.reg&7)==5)) {
      auto v=read(i.rm,64);if(!v.bits||v.kind==Kind::kOutput||!put(i.rm,{v.kind==Kind::kNative?Kind::kNative:Kind::kWord,64,0},64))return false;
      if ((view.image[i.next-1] & 63) != 0) s.flags=true;
    } else if((i.op==0x85||i.op==0x84||i.op==0x39||i.op==0x3b)) {
      const unsigned bits=i.op==0x84?8:i.width;
      if(!read(i.reg,bits).bits||!operand(bits).bits)return false;
      s.flags=true;
    } else if((i.op==0x80||i.op==0x81||i.op==0x83)&&(i.reg&7)==7) {
      if(!operand(i.op==0x80?8:i.width).bits)return false;
      s.flags=true;
    } else if(i.op==0x0f&&i.op2>=0x40&&i.op2<=0x4f&&s.flags) {
      const auto a=read(i.reg,i.width),b=operand(i.width);
      if(!a.bits||!b.bits||!put(i.reg,Meet(a,b),i.width))return false;
    } else return false;
    const auto enqueue=[&](uint32_t next)->bool {
      if(!graph.At(next))return false;
      const auto [it,inserted]=states.emplace(next,s);
      if(!inserted && it->second.sp!=s.sp)return false;
      if(inserted||MeetState(it->second,s))pending.push_back(next);
      return true;
    };
    if(i.jump&&!enqueue(i.target))return false;
    if((!i.jump||i.conditional)&&!enqueue(i.next))return false;
  }
  if(!returned||!success)return false;
  evidence.functions.push_back(fn);return true;
}
}  // namespace mfgunlock::fgpreset::cached
