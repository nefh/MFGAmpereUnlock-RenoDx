// SPDX-License-Identifier: MIT
#pragma once

// Location-independent host MFG contract. Strings identify semantic sinks;
// they never authorize a write. Every site must be a reachable instruction in
// a .pdata function and participate in the paired advertise/execution flow.
// No provider version, RVA table or code fingerprint selects a candidate.
#include "./endpoint_backend.hpp"
#include <map>
#include <set>

namespace mfgunlock::mfggate {
using Range = endpoint::ImageRange;
using View = endpoint::structural::ImageView;
inline constexpr uint32_t kArchitectureThreshold = 0x1b0;  // NVAPI architecture enum, not DLL version
inline constexpr size_t kMaxFunctionBytes = 65536;
inline constexpr size_t kMaxAnchoredFunctions = 64;
enum class Result { kUnresolved, kAmbiguous, kVerified };
inline const char* Name(Result value) {
  switch (value) {
    case Result::kAmbiguous: return "AMBIGUOUS";
    case Result::kVerified: return "STRUCTURAL_VERIFIED";
    default: return "UNRESOLVED";
  }
}
struct Instruction {
  uint32_t rva=0, next=0, target=0;
  unsigned op=0, op2=0, rex=0, width=32, reg=0, rm=0, mod=3, index=16, scale=1;
  int64_t displacement=0;
  uint64_t immediate=0;
  uint32_t immediate_rva=0;
  bool memory=false, rip=false, sib=false, call=false, jump=false, conditional=false, terminal=false;
};

// Extend the existing bounded scalar decoder locally for SSE and TLS accesses
// encountered in the MFG validation functions. Unknown encodings stop a path;
// no instruction is skipped to hunt for a byte signature.
inline size_t Length(const View& v,size_t at,size_t end) {
  if (at>=end || end>v.image.size()) return 0;
  size_t p=at; bool wide=false,op16=false,address=false;
  for (unsigned n=0;n<8 && p<end;++n) {
    const auto x=v.image[p];
    if(x>=0x40 && x<=0x4f) {wide=(x&8)!=0;++p;}
    else if(x==0x66) {op16=true;++p;}
    else if(x==0x67) {address=true;++p;}
    else if(x==0xf2 || x==0xf3 || x==0x64 || x==0x65) ++p;
    else break;
  }
  if(p>=end || address) return 0;
  const auto op=v.image[p++]; bool modrm=false;size_t imm=0;
  if((op>=0x50 && op<=0x5f)||op==0x90||op==0x98||op==0x99||op==0xc3||op==0xcc) {}
  else if(op>=0xb8 && op<=0xbf) imm=wide?8:op16?2:4;
  else if(op>=0xb0 && op<=0xb7) imm=1;
  else if((op>=0x70 && op<=0x7f)||op==0xeb||op==0x6a) imm=1;
  else if(op==0xe8||op==0xe9||op==0x68) imm=4;
  else if(op==0xc2) imm=2;
  else if(op==0x0f) {
    if(p>=end) return 0;
    const auto s=v.image[p++];
    if(s>=0x80 && s<=0x8f) imm=4;
    else if((s>=0x40&&s<=0x4f)||(s>=0x90&&s<=0x9f)||
        s==0x10||s==0x11||s==0x16||s==0x17||s==0x28||s==0x29||
        s==0x2a||s==0x2c||s==0x2e||s==0x2f||s==0x54||s==0x55||
        s==0x56||s==0x57||s==0x58||s==0x59||s==0x5a||s==0x5c||
        s==0x5d||s==0x5e||s==0x5f||s==0x6f||s==0x7f||s==0x7e||
        s==0xb6||s==0xb7||s==0xbe||s==0xbf||s==0xaf||s==0x1f) modrm=true;
    else return 0;
  } else if(op==0x25||op==0x3d||op==0x05||op==0x0d||op==0x2d||op==0xa9) imm=op16?2:4;
  else if(op==0x3c||op==0x24||op==0xa8) imm=1;
  else if(op==0x01||op==0x03||op==0x09||op==0x0b||op==0x21||op==0x23||
      op==0x29||op==0x2b||op==0x31||op==0x33||op==0x38||op==0x39||op==0x3a||op==0x3b||
      op==0x63||op==0x84||op==0x85||(op>=0x88&&op<=0x8d)||op==0xff||
      op==0xc6||op==0xc7||op==0x80||op==0x81||op==0x83||op==0xc0||op==0xc1||
      op==0x69||op==0x6b||(op>=0xd0&&op<=0xd3)||op==0xf6||op==0xf7) {
    modrm=true;
    if(op==0xc6||op==0x80||op==0x83||op==0xc0||op==0xc1||op==0x6b) imm=1;
    if(op==0xc7||op==0x81||op==0x69) imm=op16?2:4;
  } else return 0;
  if(modrm) {
    if(p>=end)return 0;
    const auto m=v.image[p++];const auto mod=m>>6,rm=m&7;
    if((op==0xf6||op==0xf7)&&((m>>3)&7)==0)imm=op==0xf6?1:op16?2:4;
    if(mod!=3&&rm==4) {
      if(p>=end)return 0;
      const auto s=v.image[p++];if(mod==0&&(s&7)==5)p+=4;
    } else if(mod==0&&rm==5)p+=4;
    if(mod==1)++p;
    if(mod==2)p+=4;
  }
  p+=imm;
  return p<=end && p-at<=15 ? p-at:0;
}
inline Instruction Decode(const View& v,uint32_t at,uint32_t end) {
  Instruction d;d.rva=at;
  const auto n=Length(v,at,end);if(!n)return d;
  d.next=at+static_cast<uint32_t>(n);size_t p=at;unsigned prefix=0;
  while(p<d.next) {
    const auto x=v.image[p];
    if(x>=0x40&&x<=0x4f)d.rex=x;
    else if(x==0x66)d.width=16;
    else if(x!=0x64&&x!=0x65&&x!=0xf2&&x!=0xf3)break;
    prefix=x;++p;
  }
  (void)prefix;
  if(d.rex&8)d.width=64;
  d.op=v.image[p++];
  if(d.op==0x0f)d.op2=v.image[p++];
  d.conditional=(d.op>=0x70&&d.op<=0x7f)||(d.op==0x0f&&d.op2>=0x80&&d.op2<=0x8f);
  d.jump=d.conditional||d.op==0xe9||d.op==0xeb;
  d.call=d.op==0xe8;
  if(d.jump||d.call) {
    const int64_t delta=d.next-p==1?static_cast<int8_t>(v.image[p]):static_cast<int32_t>(v.U32(p));
    const auto target=int64_t{d.next}+delta;
    if(target>0&&static_cast<uint64_t>(target)<v.image.size()) d.target=static_cast<uint32_t>(target);
    return d;
  }
  if(d.op==0xc3||d.op==0xc2||d.op==0xcc){d.terminal=true;return d;}
  if(d.op>=0xb8&&d.op<=0xbf) {
    d.reg=(d.op-0xb8)+((d.rex&1)?8:0);d.immediate_rva=static_cast<uint32_t>(p);
    d.immediate=d.width==64?v.U64(p):d.width==16?fatbin::ReadU16(v.image.data()+p):v.U32(p);return d;
  }
  if(d.op>=0xb0&&d.op<=0xb7) {
    d.reg=(d.op-0xb0)+((d.rex&1)?8:0);if(!d.rex&&d.reg>=4)d.reg-=4;
    d.width=8;d.immediate_rva=static_cast<uint32_t>(p);d.immediate=v.image[p];return d;
  }
  if(d.op==0x3d||d.op==0x05||d.op==0x0d||d.op==0x2d||d.op==0x25) {d.rm=0;d.immediate_rva=static_cast<uint32_t>(p);d.immediate=d.width==16?fatbin::ReadU16(v.image.data()+p):v.U32(p);return d;}
  const bool modrm=d.op==0x01||d.op==0x03||d.op==0x09||d.op==0x0b||d.op==0x21||d.op==0x23||
      d.op==0x29||d.op==0x2b||d.op==0x31||d.op==0x33||d.op==0x38||d.op==0x39||d.op==0x3a||d.op==0x3b||
      d.op==0x63||d.op==0x84||d.op==0x85||(d.op>=0x88&&d.op<=0x8d)||d.op==0xff||
      d.op==0xc6||d.op==0xc7||d.op==0x80||d.op==0x81||d.op==0x83||d.op==0xc0||d.op==0xc1||
      d.op==0x69||d.op==0x6b||(d.op>=0xd0&&d.op<=0xd3)||d.op==0xf6||d.op==0xf7||
      (d.op==0x0f&&!(d.op2>=0x80&&d.op2<=0x8f));
  if(!modrm)return d;
  const auto m=v.image[p++];d.mod=m>>6;d.reg=((m>>3)&7)+((d.rex&4)?8:0);d.rm=(m&7)+((d.rex&1)?8:0);
  d.memory=d.mod!=3;
  if(d.memory&&(m&7)==4) {d.sib=true;const auto s=v.image[p++];d.rm=(s&7)+((d.rex&1)?8:0);d.index=((s>>3)&7)+((d.rex&2)?8:0);d.scale=1u<<(s>>6);if(d.index==4)d.index=16;}
  if(d.mod==1)d.displacement=static_cast<int8_t>(v.image[p++]);
  else if(d.mod==2||(d.mod==0&&(m&7)==5)||(d.mod==0&&d.sib&&d.rm==5)) {
    d.displacement=static_cast<int32_t>(v.U32(p));p+=4;
    d.rip=d.mod==0&&!d.sib&&(m&7)==5;
  }
  if(d.rip) {const auto t=int64_t{d.next}+d.displacement;if(t>=0&&static_cast<uint64_t>(t)<v.image.size())d.target=static_cast<uint32_t>(t);}
  if(d.op==0x81||d.op==0x83||d.op==0x80||d.op==0xc6||d.op==0xc7) {
    d.immediate_rva=static_cast<uint32_t>(p);
    d.immediate=d.op==0x81||d.op==0xc7?(d.width==16?fatbin::ReadU16(v.image.data()+p):v.U32(p)):v.image[p];
  }
  if(d.op==0xff) {d.call=(d.reg&7)==2;d.terminal=(d.reg&7)==4;}
  return d;
}
struct Graph {
  uint32_t begin=0,end=0;
  std::map<uint32_t,Instruction> instructions;
  bool overlap=false;
  unsigned stopped=0;
  const Instruction* At(uint32_t at) const {
    const auto it=instructions.find(at);return it==instructions.end()?nullptr:&it->second;
  }
  const Instruction* Next(const Instruction* i) const {return i?At(i->next):nullptr;}
  const Instruction* Previous(uint32_t at) const {
    auto it=instructions.lower_bound(at);if(it==instructions.begin())return nullptr;
    --it;return it->second.next==at?&it->second:nullptr;
  }
  bool Build(const View& v,std::pair<uint32_t,uint32_t> f) {
    begin=f.first;end=f.second;
    if(begin>=end||end-begin>kMaxFunctionBytes||!v.InSection(begin,end-begin,true))return false;
    std::vector<int32_t> owner(end-begin,-1);std::vector<uint32_t> pending{begin};
    while(!pending.empty()) {
      const auto at=pending.back();pending.pop_back();
      if(at<begin||at>=end){++stopped;continue;}
      if(instructions.contains(at))continue;
      auto d=Decode(v,at,end);if(!d.next){++stopped;continue;}
      for(uint32_t b=at;b<d.next;++b) {
        if(owner[b-begin]!=-1&&owner[b-begin]!=static_cast<int32_t>(at)) {overlap=true;return false;}
        owner[b-begin]=static_cast<int32_t>(at);
      }
      instructions.emplace(at,d);
      if(d.jump) {if(d.target>=begin&&d.target<end)pending.push_back(d.target);else ++stopped;}
      if(!d.terminal&&(!d.jump||d.conditional)&&d.next<end)pending.push_back(d.next);
    }
    return !overlap;
  }
};
struct Witness {uint32_t rva=0; std::vector<unsigned char> bytes;};
struct Site { uint32_t instruction=0, immediate=0; unsigned length=0; };
struct Plan {
  Result result=Result::kUnresolved;
  std::string reason="NO_STRUCTURAL_ATTEMPT";
  std::array<Site,2> sites{};
  uint32_t ceiling=0,flag_offset=0,query_slot=0;
  std::array<uint32_t,4> functions{}; // advertise, create, evaluate, validator
  unsigned candidates=0;
  std::vector<Witness> witnesses;
};
inline bool IsMovReg(const Instruction* i,unsigned dst,unsigned src,unsigned width=64) {
  return i&&i->width==width&&!i->memory&&((i->op==0x8b&&i->reg==dst&&i->rm==src)||
      (i->op==0x89&&i->rm==dst&&i->reg==src));
}
inline bool IsImmMov(const Instruction* i,unsigned& dst,uint32_t& value) {
  if(!i||i->width!=32||i->op<0xb8||i->op>0xbf)return false;
  dst=i->reg;value=static_cast<uint32_t>(i->immediate);return true;
}
inline bool IsCmpArch(const Instruction& i) {
  return i.width==32&&!i.memory&&i.immediate==kArchitectureThreshold&&
      (i.op==0x3d||(i.op==0x81&&(i.reg&7)==7));
}
inline bool WritesRegister(const Instruction& d,unsigned r);
inline bool LinearEdge(const Graph& g,const Instruction* a,const Instruction* b) {
  if(!a||!b||a->next!=b->rva||a->terminal||a->jump)return false;
  for(const auto& [at,i]:g.instructions)
    if(at!=a->rva&&i.jump&&i.target==b->rva)return false;
  return true;
}
inline bool LinearSlice(const Graph& g,std::initializer_list<const Instruction*> instructions) {
  const Instruction* prev=nullptr;
  for(const auto* i:instructions){if(!i||(prev&&!LinearEdge(g,prev,i)))return false;prev=i;}
  return true;
}
inline bool HasThisAlias(const Graph& g,unsigned reg,uint32_t before) {
  // Reverse all incoming edges. Each reaching definition must be this->alias;
  // caller-clobbered registers and arithmetic invalidations cannot masquerade
  // as the same object merely because an early MOV once used RCX.
  std::vector<uint32_t> pending{before};std::set<uint32_t> seen;bool initialized=false;
  while(!pending.empty()) {
    const auto node=pending.back();pending.pop_back();if(!seen.insert(node).second)continue;
    if(node==g.begin){if(reg!=1)return false;initialized=true;continue;}
    bool predecessor=false;
    for(const auto& [at,i]:g.instructions) {
      if(!(i.jump&&i.target==node)&&!(!i.terminal&&(!i.jump||i.conditional)&&i.next==node))continue;
      predecessor=true;
      if(WritesRegister(i,reg)) {
        if(at-g.begin>192||!IsMovReg(&i,reg,1))return false;
        // This initial RCX must not have been overwritten before the alias.
        for(const auto& [p,d]:g.instructions)if(p<at&&WritesRegister(d,1))return false;
        initialized=true;
      } else pending.push_back(at);
    }
    if(!predecessor)return false;
  }
  return initialized;
}
inline uint32_t DispatchSlot(const View& v) {
  if(!v.Fits(0x3c,4))return 0;
  const size_t opt=v.U32(0x3c)+24;
  if(!v.Fits(opt,200))return 0;
  const auto lc=v.U32(opt+192),n=v.U32(opt+196);
  if(n<128||!v.InSection(lc,128,false)||v.U32(lc)<128)return 0;
  const auto ptr=v.U64(lc+120),preferred=v.U64(opt+24);
  for(auto base:{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v.image.data())),preferred}) {
    if(ptr>=base&&ptr-base<=UINT32_MAX&&v.InSection(ptr-base,8,false))return static_cast<uint32_t>(ptr-base);
  }
  return 0;
}
inline bool RaxDispatch(const View& v,const Instruction* call) {
  if(!call||call->op!=0xff||!call->call)return false;
  if(!call->memory)return call->rm==0;
  const auto slot=DispatchSlot(v);
  return slot&&call->rip&&call->target==slot;
}
// Reaching-definition checks used for inputs to the count validator. Stop at
// actual definitions, not at a conveniently nearby MOV with the same register.
inline bool PreservedArgument(const Graph& g,unsigned reg,uint32_t at) {
  std::vector<uint32_t> todo{at};std::set<uint32_t> seen;bool entry=false;
  while(!todo.empty()) {
    auto node=todo.back();todo.pop_back();if(!seen.insert(node).second)continue;
    if(node==g.begin){entry=true;continue;}
    bool pred=false;
    for(const auto& [p,d]:g.instructions) {
      if(!(d.jump&&d.target==node)&&!(!d.terminal&&(!d.jump||d.conditional)&&d.next==node))continue;
      pred=true;if(WritesRegister(d,reg))return false;todo.push_back(p);
    }
    if(!pred)return false;
  }
  return entry;
}
inline bool CountField(const Graph& g,unsigned reg,uint32_t at,uint32_t& field) {
  std::vector<uint32_t> todo{at};std::set<uint32_t> seen;bool defined=false;
  while(!todo.empty()) {
    auto node=todo.back();todo.pop_back();if(!seen.insert(node).second)continue;
    if(node==g.begin)return false;
    bool pred=false;
    for(const auto& [p,d]:g.instructions) {
      if(!(d.jump&&d.target==node)&&!(!d.terminal&&(!d.jump||d.conditional)&&d.next==node))continue;
      pred=true;
      if(WritesRegister(d,reg)) {
        if(d.op!=0x8b||d.width!=32||!d.memory||d.rip||d.sib||d.displacement<=0||d.displacement>65536||
            !HasThisAlias(g,d.rm,p))return false;
        const auto offset=static_cast<uint32_t>(d.displacement);
        if(defined&&field!=offset)return false;
        field=offset;defined=true;
      }else todo.push_back(p);
    }
    if(!pred)return false;
  }
  return defined;
}
inline bool StringRef(const View& v,const Instruction& i,std::string_view value) {
  return i.op==0x8d&&i.width==64&&i.rip&&v.String(i.target,256)==value;
}
// Raw LEA is only a cheap candidate prefilter. Graph::At/Decode and the full
// semantic witnesses below must independently qualify every selected reference.
inline std::vector<Graph> Anchored(const View& v,std::string_view name) {
  std::set<uint32_t> addresses,functions;
  for(const auto& r:v.ranges) {
    if(r.executable||!v.Fits(r.rva,r.bytes))continue;
    auto first=v.image.begin()+r.rva,last=first+r.bytes;
    while(first!=last) {
      const auto hit=std::search(first,last,name.begin(),name.end());if(hit==last)break;
      const auto at=static_cast<uint32_t>(hit-v.image.begin());
      if(v.String(at,256)==name)addresses.insert(at);
      if(addresses.size()>kMaxAnchoredFunctions)return {};
      first=hit+1;
    }
  }
  if(addresses.empty())return {};
  for(const auto& f:v.functions) {
    if(f.second-f.first>kMaxFunctionBytes)continue;
    for(uint32_t at=f.first;at+7<=f.second;++at) {
      if((v.image[at]!=0x48&&v.image[at]!=0x4c)||v.image[at+1]!=0x8d||(v.image[at+2]&0xc7)!=5)continue;
      const auto target=v.Relative(at,3,7);
      if(addresses.contains(target)) {functions.insert(f.first);break;}
    }
  }
  if(functions.size()>kMaxAnchoredFunctions)return {};
  std::vector<Graph> out;
  for(auto f:functions) {
    Graph g;if(!g.Build(v,v.Function(f)))continue;
    bool found=false;for(const auto& [at,i]:g.instructions){(void)at;found|=StringRef(v,i,name);}
    if(found)out.push_back(std::move(g));
  }
  return out;
}
struct Advertise { Site site;uint32_t ceiling=0;unsigned count_reg=0; };
inline std::vector<Advertise> Advertisements(const View& v,const Graph& g) {
  std::vector<Advertise> out;
  for(const auto& [at,i]:g.instructions) {
    if(!IsCmpArch(i))continue;
    const auto* branch=g.Next(&i);
    if(!branch||!branch->conditional||!((branch->op==0x0f&&branch->op2==0x8c)||branch->op==0x7c))continue;
    const auto* high=g.Next(branch);const auto* low=g.At(branch->target);
    unsigned highreg=0,lowreg=0;uint32_t highvalue=0,lowvalue=0;
    if(!IsImmMov(high,highreg,highvalue)||!IsImmMov(low,lowreg,lowvalue)||
        highreg!=lowreg||lowvalue!=1||highvalue<=1||highvalue>32||low->rva<=high->rva)continue;
    // Shared NGX Set(uint32) publication consumes the same register on both
    // sides. The branch target and merge must be real instruction boundaries.
    auto* a=g.Next(low);auto* b=g.Next(a);auto* key=g.Next(b);auto* obj=g.Next(key);auto* slot=g.Next(obj);auto* call=g.Next(slot);
    if(!a||a->op!=0x8b||a->reg!=0||!a->memory||a->rip||a->sib||a->displacement!=0||
        !IsMovReg(b,8,highreg,32)||!key||key->reg!=2||!StringRef(v,*key,"DLSSG.MultiFrameCountMax")||
        !IsMovReg(obj,1,a->rm)||!slot||slot->op!=0x8b||slot->reg!=0||slot->rm!=0||
        !slot->memory||slot->sib||slot->rip||slot->width!=64||slot->displacement!=0x20||!RaxDispatch(v,call)||
        !LinearSlice(g,{a,b,key,obj,slot,call})||!LinearEdge(g,&i,branch))continue;
    // Default high-count must reach that shared publication. All direct branch
    // targets inside this slice must decode; escaping the slice is rejected.
    bool reachable=false,valid=true;std::set<uint32_t> seen;std::vector<uint32_t> pending{high->rva};
    while(!pending.empty()&&valid) {
      auto x=pending.back();pending.pop_back();if(x==a->rva){reachable=true;continue;}
      if(!seen.insert(x).second)continue;
      auto* n=g.At(x);if(!n||x<high->rva||x>=a->rva||n->terminal){valid=false;break;}
      if(n->jump) {pending.push_back(n->target);if(n->conditional)pending.push_back(n->next);}
      else pending.push_back(n->next);
    }
    if(!valid||!reachable)continue;
    out.push_back({{at,i.immediate_rva,static_cast<unsigned>(i.next-at)},highvalue,highreg});
  }
  return out;
}
struct Execution {Site site;uint32_t flag=0,query_slot=0;unsigned object=0;};
inline std::vector<Execution> Executions(const View& v,const Graph& g) {
  std::vector<Execution> out;
  for(const auto& [at,a]:g.instructions) {
    if(a.op!=0x8b||a.width!=64||a.reg!=1||!a.memory||a.rip||a.sib||!HasThisAlias(g,a.rm,at))continue;
    const auto* vt=g.Next(&a);const auto* getter=g.Next(vt);const auto* call=g.Next(getter);
    const auto* cmp=g.Next(call);const auto* set=g.Next(cmp);const auto* store=g.Next(set);
    if(!vt||vt->op!=0x8b||vt->width!=64||vt->reg!=0||vt->rm!=1||!vt->memory||vt->displacement!=0||
        !getter||getter->op!=0x8b||getter->width!=64||getter->reg!=0||getter->rm!=0||!getter->memory||
        getter->displacement<=0||getter->displacement>512||getter->displacement%8||
        !RaxDispatch(v,call)||!cmp||!IsCmpArch(*cmp)||cmp->rm!=0||
        !LinearSlice(g,{&a,vt,getter,call,cmp,set,store})||
        !set||set->op!=0x0f||set->op2!=0x93||set->memory||set->rm!=0||
        !store||store->op!=0x88||!store->memory||store->sib||store->rip||store->reg!=0||store->rm!=a.rm||
        store->displacement<=0||store->displacement>4096)continue;
    out.push_back({{cmp->rva,cmp->immediate_rva,static_cast<unsigned>(cmp->next-cmp->rva)},
        static_cast<uint32_t>(store->displacement),static_cast<uint32_t>(getter->displacement),a.rm});
  }
  return out;
}
inline bool ValidateCountFlow(const View& v,const Graph& g,uint32_t ceiling) {
  bool named=false;
  for(const auto& [at,i]:g.instructions){(void)at;named|=StringRef(v,i,"EndpointCoreInputs::ComputeAndValidateTimeFactor");}
  if(!named)return false;
  for(const auto& [at,i]:g.instructions) {
    (void)at;
    if(i.op!=0x84||i.memory||i.reg!=2||i.rm!=2||!PreservedArgument(g,2,i.rva))continue; // boolean arg DL
    const auto* branch=g.Next(&i);if(!branch||!branch->conditional||!LinearEdge(g,&i,branch)||!((branch->op==0x0f&&branch->op2==0x84)||branch->op==0x74))continue;
    const auto* high=g.Next(branch);unsigned limitreg=0;uint32_t limit=0;
    if(!IsImmMov(high,limitreg,limit)||limit!=ceiling)continue;
    const auto* low=g.At(branch->target);const auto* accept=g.Next(low);
    if(!low||low->op!=0x83||low->memory||(low->reg&7)!=7||low->immediate!=1||
        !accept||!accept->conditional||!g.At(accept->target)||!((accept->op==0x0f&&accept->op2==0x84)||accept->op==0x74))continue;
    const unsigned countreg=low->rm;
    uint32_t countfield=0;const bool countload=CountField(g,countreg,low->rva,countfield);bool highcompare=false;
    for(const auto& [p,d]:g.instructions) {

      if(p>high->rva&&p<low->rva&&!d.memory&&d.width==32&&
          ((d.op==0x3b&&d.reg==countreg&&d.rm==limitreg)||(d.op==0x39&&d.rm==countreg&&d.reg==limitreg))) {
        auto* br=g.Next(&d);
        uint32_t highfield=0;
        if(br&&br->conditional&&LinearEdge(g,&d,br)&&CountField(g,countreg,p,highfield)&&highfield==countfield&&
            ((br->op==0x0f&&br->op2==0x86)||br->op==0x76)&&g.At(br->target)) {
          if(br->target==accept->target)highcompare=true;
          else {
            const auto* index=g.At(accept->target);const auto* one=g.Next(index);const auto* join=g.Next(one);
            if(index&&index->op==0x8b&&index->memory&&index->width==32&&index->reg==0&&index->rm==1&&
                one&&one->op==0x83&&!one->memory&&(one->reg&7)==7&&one->rm==0&&one->immediate==1&&
                join&&join->conditional&&((join->op==0x0f&&join->op2==0x84)||join->op==0x74)&&join->target==br->target)highcompare=true;
          }
        }
      }
    }
    if(countload&&highcompare)return true;
  }
  return false;
}
inline bool GetterCall(const View& v,const Graph& g,const Instruction& call,uint32_t slot) {
  const auto* load=g.Previous(call.rva);const auto* vt=load?g.Previous(load->rva):nullptr;
  return RaxDispatch(v,&call)&&LinearSlice(g,{vt,load,&call})&&load&&load->op==0x8b&&load->width==64&&
      load->reg==0&&load->rm==0&&load->memory&&!load->sib&&!load->rip&&load->displacement==slot&&
      vt&&vt->op==0x8b&&vt->width==64&&vt->reg==0&&vt->rm==1&&vt->memory&&vt->displacement==0;
}

// Bounded, non-executing scalar interpretation of an architecture enum mapper.
// Only reads tables inside the same function/leaf extent; never invokes a call
// or follows arbitrary process pointers. An unknown instruction is a rejection.
// Used to relate the advertised compare operand to the GPU query also consumed
// by EndpointCore::Create, not to guess capabilities from an unrelated cmp.
inline bool MapEnum(const View& v,uint32_t begin,uint32_t end,uint32_t stop,
                    unsigned input_reg,uint32_t input,uint32_t& output,uint32_t zero_registers=0) {
  std::array<uint64_t,16> r{};std::array<bool,16> known{};
  for(unsigned i=0;i<16;++i)known[i]=(zero_registers&(1u<<i))!=0;
  r[input_reg]=input;known[input_reg]=true;known[4]=true;
  int64_t signed_diff=0;bool equal=false,below=false,flags=false;
  uint32_t at=begin;
  for(unsigned step=0;step<80;++step) {
    if(stop&&at==stop) {if(!known[0])return false;output=static_cast<uint32_t>(r[0]);return true;}
    auto d=Decode(v,at,end);if(!d.next||at<begin||at>=end||d.width==16)return false;
    auto put=[&](unsigned n,uint64_t x){r[n]=d.width==64?x:static_cast<uint32_t>(x);known[n]=true;};
    auto address=[&](uint64_t& x)->bool {
      if(d.rip) {x=d.target;return true;}
      if(!known[d.rm]||(d.index!=16&&!known[d.index]))return false;
      x=r[d.rm]+(d.index==16?0:r[d.index]*d.scale)+d.displacement;return true;
    };
    auto source=[&](uint64_t& x,unsigned bytes)->bool {
      if(!d.memory){if(!known[d.rm])return false;x=r[d.rm];return true;}
      uint64_t a=0;if(!address(a)||a<begin||a>end||bytes>end-a||!v.Fits(a,bytes))return false;
      x=bytes==1?v.image[a]:bytes==4?v.U32(a):v.U64(a);return true;
    };
    if(d.op==0xc3) {if(!known[0])return false;output=static_cast<uint32_t>(r[0]);return true;}
    if(d.call)return false;
    if(d.op>=0xb8&&d.op<=0xbf)put(d.reg,d.immediate);
    else if(d.op==0x8b&&!d.memory) {if(!known[d.rm])return false;put(d.reg,r[d.rm]);}
    else if(d.op==0x89&&!d.memory){if(!known[d.reg])return false;put(d.rm,r[d.reg]);}
    else if(d.op==0x8d){uint64_t x=0;if(!address(x))return false;put(d.reg,x);}
    else if(d.op==0x63&&!d.memory){if(!known[d.rm])return false;put(d.reg,static_cast<int64_t>(static_cast<int32_t>(r[d.rm])));}
    else if(d.op==0x98&&d.width==64){if(!known[0])return false;r[0]=static_cast<int64_t>(static_cast<int32_t>(r[0]));}
    else if(d.op==0x03&&!d.memory){if(!known[d.reg]||!known[d.rm])return false;put(d.reg,r[d.reg]+r[d.rm]);flags=false;}
    else if(d.op==0x05){if(!known[0])return false;put(0,r[0]+d.immediate);flags=false;}
    else if(d.op==0x31||d.op==0x33) {if(d.memory||d.rm!=d.reg)return false;put(d.reg,0);flags=false;}
    else if((d.op==0x8b&&d.memory)||(d.op==0x0f&&d.op2==0xb6)){
      uint64_t x=0;if(!source(x,d.op2==0xb6?1:4))return false;put(d.reg,x);
    }
    else if((d.op==0x81||d.op==0x83)&&!d.memory&&(d.reg&7)==7) {
      if(!known[d.rm])return false;
      const auto x=static_cast<uint32_t>(r[d.rm]),y=static_cast<uint32_t>(d.immediate);
      equal=x==y;below=x<y;signed_diff=int64_t{static_cast<int32_t>(x)}-static_cast<int32_t>(y);flags=true;
    }
    else if(d.op==0x3d){if(!known[0])return false;auto x=static_cast<uint32_t>(r[0]),y=static_cast<uint32_t>(d.immediate);equal=x==y;below=x<y;signed_diff=int64_t{static_cast<int32_t>(x)}-static_cast<int32_t>(y);flags=true;}
    else if(d.op==0x0f&&d.op2==0x4c&&!d.memory){if(!flags||!known[d.reg]||!known[d.rm])return false;if(signed_diff<0)put(d.reg,r[d.rm]);}
    else if(d.op==0x83&&!d.memory&&d.rm==4&&((d.reg&7)==0||(d.reg&7)==5)) { flags=false; /* stack-only prologue/epilogue */ }
    else if(d.conditional){
      if(!flags)return false;
      const unsigned cc=d.op==0x0f?d.op2&15:d.op&15;
      bool take=false;if(cc==7)take=!below&&!equal;else if(cc==4)take=equal;else if(cc==5)take=!equal;else return false;
      if(take){at=d.target;continue;}
    }
    else if(d.op==0xe9||d.op==0xeb){at=d.target;continue;}
    else if(d.op==0xff&&d.terminal&&!d.memory){if(!known[d.rm]||r[d.rm]>UINT32_MAX)return false;at=static_cast<uint32_t>(r[d.rm]);continue;}
    else if(d.op==0x90||(d.op==0x0f&&d.op2==0x1f)){}
    else return false;
    at=d.next;
  }
  return false;
}
inline bool EnumMapper(const View& v,uint32_t start,uint32_t end,uint32_t stop,unsigned inputreg,uint32_t zero_registers=0) {
  // Finite hardware-enum contract including all current architecture IDs plus
  // invalid/default inputs. This is not a general x64 emulator or GPU trial.
  for(uint32_t x=0x140;x<=0x1b0;++x) {
    uint32_t y=0;if(!MapEnum(v,start,end,stop,inputreg,x,y,zero_registers))return false;
    if(x==0x140||x==0x160||x==0x170||x==0x180||x==0x190||x==0x1a0||x==0x1b0){if(y!=x)return false;}
    else if(y!=0x140&&y!=0x07ffffff)return false;
  }
  for(uint32_t x:{0u,0x13fu,0x1b1u,0x7fffffffu,0xffffffffu}){
    uint32_t y=0;if(!MapEnum(v,start,end,stop,inputreg,x,y,zero_registers))return false;
    if(y!=(static_cast<int32_t>(x)<0x140?0u:0x07ffffffu))return false;
  }
  return true;
}
inline std::pair<uint32_t,uint32_t> CallableExtent(const View& v,uint32_t target) {
  const auto f=v.Function(target);if(f.first)return f;
  for(const auto& r:v.ranges) if(r.executable&&target>=r.rva&&target-r.rva<r.bytes) {
    uint32_t end=static_cast<uint32_t>(std::min<uint64_t>(uint64_t{target}+1024,uint64_t{r.rva}+r.bytes));
    for(const auto& fn:v.functions){if(target>=fn.first&&target<fn.second)return {};if(fn.first>target){end=(std::min)(end,fn.first);break;}}
    return {target,end};
  }
  return {};
}
inline bool WritesRegister(const Instruction& d,unsigned r) {
  if(d.call)return r==0||r==1||r==2||r==8||r==9||r==10||r==11;
  if(d.op>=0xb0&&d.op<=0xbf)return d.reg==r;
  if(d.op>=0x58&&d.op<=0x5f)return (d.op-0x58+((d.rex&1)?8:0))==r;
  if(d.op==0x99)return r==2;
  if(d.op==0x98||d.op==0x05||d.op==0x0d||d.op==0x24||d.op==0x25||d.op==0x2d)return r==0;
  if(d.op==0x8b||d.op==0x8a||d.op==0x8d||d.op==0x33||d.op==0x03||d.op==0x0b||d.op==0x2b||
      d.op==0x63||d.op==0x23||d.op==0x69||d.op==0x6b||
      (d.op==0x0f&&((d.op2>=0x40&&d.op2<=0x4f)||d.op2==0xb6||d.op2==0xb7||d.op2==0xbe||
       d.op2==0xbf||d.op2==0xaf||d.op2==0x2c)))return d.reg==r;
  if((d.op==0xf6||d.op==0xf7)&&(d.reg&7)>=4)return r==0||r==2;
  if(!d.memory&&(d.op==0x89||d.op==0x88||d.op==0x31||d.op==0x01||d.op==0x09||d.op==0x21||d.op==0x29||
      d.op==0xc6||d.op==0xc7||d.op==0xc0||d.op==0xc1||(d.op>=0xd0&&d.op<=0xd3)||
      (d.op==0xff&&(d.reg&7)<=1)||((d.op==0xf6||d.op==0xf7)&&(d.reg&7)>=2)||
      ((d.op==0x80||d.op==0x81||d.op==0x83)&&(d.reg&7)!=7)||
      (d.op==0x0f&&d.op2>=0x90&&d.op2<=0x9f)||
      (d.op==0x0f&&d.op2==0x7e)))return d.rm==r||((d.op==0x88||d.op==0x0f)&&!d.rex&&d.rm>=4&&d.rm<8&&d.rm-4==r);
  return false;
}
inline bool ZeroBefore(const Graph& g,uint32_t at,unsigned reg) {
  std::vector<uint32_t> pending{at};std::set<uint32_t> seen;bool initialized=false;
  while(!pending.empty()) {
    const auto node=pending.back();pending.pop_back();if(!seen.insert(node).second)continue;
    if(node==g.begin)return false;
    bool predecessor=false;
    for(const auto& [p,d]:g.instructions) {
      if(!(d.jump&&d.target==node)&&!(!d.terminal&&(!d.jump||d.conditional)&&d.next==node))continue;
      predecessor=true;
      if(WritesRegister(d,reg)) {
        if(((d.op==0x33||d.op==0x31)&&!d.memory&&d.rm==d.reg)||
            (d.op==0x8d&&d.rip&&d.target==0&&d.width==64)||
            (d.op>=0xb8&&d.op<=0xbf&&d.immediate==0))initialized=true;
        else return false;
      } else pending.push_back(p);
    }
    if(!predecessor)return false;
  }
  return initialized;
}
inline bool ArchitectureOperand(const View& v,const Graph& g,const Site& site,uint32_t slot,
                                std::vector<std::pair<uint32_t,uint32_t>>& helpers) {
  const auto* compare=g.At(site.instruction);if(!compare)return false;
  const unsigned acc=compare->rm;
  if(acc!=3&&acc!=5&&acc!=6&&acc!=7&&acc<12)return false; // survives opaque ABI calls
  auto returned=[&](const Instruction* call)->bool {
    if(!call||call->op!=0xe8)return false;
    const auto f=CallableExtent(v,call->target);if(!f.first)return false;
    Graph helper;if(!helper.Build(v,f))return false;
    const auto* previous=g.Previous(call->rva);
    if(IsMovReg(previous,1,0,32)) {
      const auto* query=g.Previous(previous->rva);
      if(query&&GetterCall(v,g,*query,slot)&&EnumMapper(v,f.first,f.second,0,1)){helpers.push_back(f);return true;}
    }
    for(const auto& [at,i]:helper.instructions){(void)at;if(GetterCall(v,helper,i,slot)&&EnumMapper(v,i.next,f.second,0,0)){helpers.push_back(f);return true;}}
    return false;
  };
  unsigned sources=0,definitions=0;
  for(const auto& [at,i]:g.instructions) {
    if(at>=site.instruction)break;
    const bool mov=IsMovReg(&i,acc,0,32);
    const bool cmov=i.op==0x0f&&i.op2==0x4f&&!i.memory&&i.reg==acc&&i.rm==0&&i.width==32;
    const bool init=i.op==0x8b&&!i.memory&&i.reg==acc&&i.width==32;
    const bool writes=WritesRegister(i,acc);
    if(!writes)continue;
    ++definitions;const auto* prev=g.Previous(at);
    if(init&&!mov&&prev&&prev->op==0x33&&!prev->memory&&prev->rm==prev->reg&&prev->reg==i.rm)continue;
    if(mov&&LinearEdge(g,prev,&i)&&returned(prev)){++sources;continue;}
    if(cmov&&prev&&prev->op==0x3b&&!prev->memory&&prev->reg==0&&prev->rm==acc&&returned(g.Previous(prev->rva))){++sources;continue;}
    // Inlined enum mapper followed by signed max reduction into the same accumulator.
    if(mov&&prev&&prev->op==0x0f&&prev->op2==0x4e&&!prev->memory&&prev->reg==0&&prev->rm==acc) {
      const auto* reduce=g.Previous(prev->rva);
      if(!reduce||reduce->op!=0x3b||reduce->reg!=0||reduce->rm!=acc||reduce->memory)return false;
      bool found=false;
      for(const auto& [q,j]:g.instructions) if(q<reduce->rva&&reduce->rva-q<256&&GetterCall(v,g,j,slot)){
        uint32_t zero=0;for(unsigned r=0;r<16;++r)if(ZeroBefore(g,j.next,r))zero|=1u<<r;
        if(EnumMapper(v,j.next,g.end,reduce->rva,0,zero)){found=true;break;}
      }
      if(found){++sources;continue;}
    }
    return false;
  }
  return sources>0&&definitions>=sources;
}
inline void AddWitness(Plan& plan,const View& v,uint32_t at,size_t n) {
  if(!v.Fits(at,n)||!n)return;
  plan.witnesses.push_back({at,{v.image.begin()+at,v.image.begin()+at+n}});
}
inline void AddGraph(Plan& p,const View& v,const Graph& g) {
  // Full function regions are snapshots for TOCTOU, never matching signatures.
  AddWitness(p,v,g.begin,g.end-g.begin);
  for(const auto& [at,i]:g.instructions) {
    (void)at;
    if(i.op==0x8d&&i.rip) {const auto s=v.String(i.target,256);if(!s.empty())AddWitness(p,v,i.target,s.size()+1);}
  }
}
inline bool FindPlan(std::span<const unsigned char> image,std::span<const Range> ranges,Plan& plan) {
  plan={};View v{image,ranges,{}};
  if(!v.Fits(0,64)||image[0]!='M'||image[1]!='Z') {plan.reason="INVALID_PE";return false;}
  const size_t nt=v.U32(0x3c),opt=nt+24;
  if(!v.Fits(nt,264)||v.U32(nt)!=0x4550||fatbin::ReadU16(image.data()+nt+4)!=0x8664||
      fatbin::ReadU16(image.data()+opt)!=0x20b||v.U32(opt+56)!=image.size()||!v.ReadFunctions()) {
    plan.reason="INVALID_PE_OR_FUNCTION_TABLE";return false;
  }
  auto caps=Anchored(v,"DLSSG.MultiFrameCountMax");
  auto creates=Anchored(v,"EndpointCore::Create");
  auto evaluates=Anchored(v,"EndpointCore::Evaluate");
  if(caps.empty()||creates.empty()||evaluates.empty()) {plan.reason="SEMANTIC_ANCHOR_NOT_RESOLVED";return false;}
  std::vector<Plan> verified;
  for(const auto& cg:caps)for(const auto& cap:Advertisements(v,cg))
    for(const auto& rg:creates)for(const auto& runtime:Executions(v,rg)) {
      ++plan.candidates;
      std::vector<std::pair<uint32_t,uint32_t>> helpers;
      if(!ArchitectureOperand(v,cg,cap.site,runtime.query_slot,helpers))continue;
      for(const auto& eg:evaluates)for(const auto& [at,i]:eg.instructions) {
        if(i.op!=0x0f||i.op2!=0xb6||!i.memory||i.sib||i.rip||i.reg!=2||i.displacement!=runtime.flag||
            !HasThisAlias(eg,i.rm,at))continue;
        const auto* arg=eg.Next(&i);const auto* call=eg.Next(arg);
        if(!arg||arg->op!=0x8d||arg->width!=64||arg->reg!=1||!call||call->op!=0xe8||!call->target||
            !LinearSlice(eg,{&i,arg,call}))continue;
        Graph vg;if(!vg.Build(v,v.Function(call->target))||!ValidateCountFlow(v,vg,cap.ceiling))continue;
        Plan p;p.result=Result::kVerified;p.reason="ADVERTISE_EXECUTE_COUNT_CONTRACT";
        p.sites={cap.site,runtime.site};p.ceiling=cap.ceiling;p.flag_offset=runtime.flag;p.query_slot=runtime.query_slot;
        p.functions={cg.begin,rg.begin,eg.begin,vg.begin};
        AddWitness(p,v,0,static_cast<size_t>(v.U32(opt+60)));
        AddWitness(p,v,v.U32(opt+136),v.U32(opt+140));
        const auto lc=v.U32(opt+192);if(lc&&v.InSection(lc,128,false))AddWitness(p,v,lc,128);
        if(const auto slot=DispatchSlot(v))AddWitness(p,v,slot,8);
        AddGraph(p,v,cg);AddGraph(p,v,rg);AddGraph(p,v,eg);AddGraph(p,v,vg);
        for(const auto& h:helpers)AddWitness(p,v,h.first,h.second-h.first);
        verified.push_back(std::move(p));
      }
    }
  if(verified.size()!=1) {
    plan.result=verified.empty()?Result::kUnresolved:Result::kAmbiguous;
    plan.reason=verified.empty()?"ADVERTISE_EXECUTE_FLOW_NOT_PROVEN":"MULTIPLE_SEMANTIC_PLANS";
    return false;
  }
  auto attempts=plan.candidates;plan=std::move(verified.front());plan.candidates=attempts;return true;
}
inline bool ValidatePlan(std::span<const unsigned char> image,const Plan& plan) {
  if(plan.result!=Result::kVerified||plan.witnesses.empty()||plan.sites[0].instruction==plan.sites[1].instruction)return false;
  for(const auto& w:plan.witnesses) {
    if(w.rva>image.size()||w.bytes.size()>image.size()-w.rva||
        !std::equal(w.bytes.begin(),w.bytes.end(),image.begin()+w.rva))return false;
  }
  for(const auto& s:plan.sites) {
    if(s.length<5||s.length>7||s.instruction>image.size()||s.length>image.size()-s.instruction||
        s.immediate<s.instruction||static_cast<uint64_t>(s.immediate)+4>static_cast<uint64_t>(s.instruction)+s.length||
        fatbin::ReadU32(image.data()+s.immediate)!=kArchitectureThreshold)return false;
  }
  const auto& a=plan.sites[0];const auto& b=plan.sites[1];
  if(a.instruction<b.instruction+b.length&&b.instruction<a.instruction+a.length)return false;
  return true;
}
}  // namespace mfgunlock::mfggate
