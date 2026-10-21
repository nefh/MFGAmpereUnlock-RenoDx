// SPDX-License-Identifier: MIT
#pragma once

// The flat-network generation has one DX and one Vulkan network, not DL1/DL2.
// This contract selects an existing PTX implementation only after proving its
// constructor and loader equivalent to the native implementation, except for
// the bound vtable and payload/length arguments. Relative operands are decoded;
// neither DLL identity nor an expected RVA/hash authorizes a write.
#include "./mfg_gate.hpp"

namespace mfgunlock::endpoint {
namespace flat {
using View=structural::ImageView;
using Graph=mfggate::Graph;
using Ins=mfggate::Instruction;
inline bool Mem(const Ins* i,unsigned op,unsigned reg,unsigned base,int64_t disp,unsigned width=64) {
  return i&&i->op==op&&i->width==width&&i->memory&&!i->rip&&!i->sib&&
      i->reg==reg&&i->rm==base&&i->displacement==disp;
}
inline bool Imm(const Ins* i,unsigned reg,uint32_t value) {
  unsigned r=0;uint32_t v=0;return mfggate::IsImmMov(i,r,v)&&r==reg&&v==value;
}
inline bool Zero(const Ins* i,unsigned reg) {
  return i&&i->op==0x33&&!i->memory&&i->width==32&&i->reg==reg&&i->rm==reg;
}
inline bool Call(const Ins* i,uint32_t target) { return i&&i->call&&i->op==0xe8&&i->target==target; }
inline bool Indirect(const Ins* i,uint32_t dispatch) { return i&&i->call&&i->op==0xff&&i->rip&&i->target==dispatch; }
inline bool Test(const Ins* i,unsigned reg,unsigned width=32) {
  return i&&i->op==0x85&&!i->memory&&i->reg==reg&&i->rm==reg&&i->width==width;
}
inline bool Build(const View& v,uint32_t function,Graph& g) {
  const auto f=v.Function(function);
  return f.first&&g.Build(v,f)&&g.stopped==0&&!g.instructions.empty();
}
inline bool AddWitness(const View& v,StructuralPlan& plan,uint32_t rva,size_t bytes) {
  if(!v.Fits(rva,bytes)||bytes>65536||plan.witnesses.size()>=96)return false;
  for(const auto& w:plan.witnesses) if(w.rva==rva&&w.bytes.size()==bytes)return true;
  plan.witnesses.push_back({rva,{v.image.begin()+rva,v.image.begin()+rva+bytes}});
  return true;
}
inline bool Remember(const View& v,StructuralPlan& p,const Graph& g) {return AddWitness(v,p,g.begin,g.end-g.begin);}

// Full primary-object RTTI/COL relationship, not just a nearby class string.
inline bool TableNamed(const View& v,uint32_t table,std::string_view name,StructuralPlan* p=nullptr) {
  if(table<8||!v.InSection(table-8,8+5*8,false))return false;
  const auto col=v.PointerRva(v.U64(table-8));
  if(!col||!v.InSection(col,24,false)||v.U32(col)!=1||v.U32(col+4)||v.U32(col+8)||v.U32(col+20)!=col)return false;
  const auto type=v.U32(col+12);
  if(!v.InSection(type,16+name.size()+1,false)||v.String(type+16)!=name)return false;
  for(unsigned slot=0;slot<5;++slot) if(!v.Function(v.PointerRva(v.U64(table+slot*8))).first)return false;
  return !p||(AddWitness(v,*p,table-8,8+5*8)&&AddWitness(v,*p,col,24)&&AddWitness(v,*p,type,16+name.size()+1));
}
inline std::vector<uint32_t> Tables(const View& v,std::string_view name) {
  std::vector<uint32_t> tables;
  for(const auto& s:v.ranges) {
    if(s.executable||!v.Fits(s.rva,s.bytes))continue;
    for(size_t at=(s.rva+7u)&~size_t{7};at+48<=size_t{s.rva}+s.bytes;at+=8) {
      if(TableNamed(v,static_cast<uint32_t>(at+8),name)) {
        tables.push_back(static_cast<uint32_t>(at+8));if(tables.size()>4)return {};
      }
    }
  }
  return tables;
}
struct Arm {uint32_t constructor=0,allocator=0,allocation=0,join=0;};
// Parse allocator -> nullable ctor -> RCX result, including both null and live
// paths. No interior branch may enter an allocation arm.
inline bool ReadArm(const View& v,const Graph& g,uint32_t begin,Arm& a) {
  const Ins* i=g.At(begin);unsigned dst=0;
  if(!mfggate::IsImmMov(i,dst,a.allocation)||dst!=1||a.allocation<64||a.allocation>1024*1024)return false;
  i=g.Next(i);if(!i||!i->call||i->op!=0xe8||!v.Function(i->target).first)return false;
  a.allocator=i->target;
  i=g.Next(i);if(!i||i->op!=0x89||!i->memory||i->reg!=0||i->width!=64||i->rip||
      !((!i->sib&&i->rm==5)||(i->sib&&i->rm==4&&i->index==16)))return false;
  i=g.Next(i);if(!Test(i,0,64))return false;
  i=g.Next(i);if(!i||i->op!=0x74)return false;
  const auto null_path=i->target;
  i=g.Next(i);if(!mfggate::IsMovReg(i,1,0))return false;
  i=g.Next(i);if(!i||!i->call||i->op!=0xe8||!v.Function(i->target).first)return false;
  a.constructor=i->target;
  i=g.Next(i);if(!mfggate::IsMovReg(i,1,0))return false;
  i=g.Next(i);if(!i||i->op!=0xeb)return false;
  const auto join=i->target;
  const auto* null=g.At(null_path);
  if(!Zero(null,1)||null->next!=join)return false;
  const auto* end=g.At(join);if(!end)return false;
  a.join=end->op==0xeb?end->target:join;
  // All instructions in this arm are exactly the accepted chain above. Edges
  // from outside may reach its first instruction only.
  for(const auto& [at,d]:g.instructions)
    if(d.jump&&d.target>begin&&d.target<join&&(at<begin||at>=join))return false;
  return a.join!=0;
}
// A unique reaching definition must dominate the observation. This rejects
// a lookalike MOV on only one incoming path or a caller-clobbered register.
inline const Ins* Definition(const Graph& g,uint32_t before,unsigned reg) {
  std::vector<uint32_t> todo{before};std::set<uint32_t> seen;const Ins* result=nullptr;
  while(!todo.empty()) {
    const auto node=todo.back();todo.pop_back();if(!seen.insert(node).second)continue;
    if(node==g.begin)return nullptr;
    bool pred=false;
    for(const auto& [at,d]:g.instructions) {
      if(!(d.jump&&d.target==node)&&!(!d.terminal&&(!d.jump||d.conditional)&&d.next==node))continue;
      pred=true;
      if(mfggate::WritesRegister(d,reg)) {if(result&&result->rva!=at)return nullptr;result=&d;}
      else todo.push_back(at);
    }
    if(!pred)return nullptr;
  }
  return result;
}
inline bool Dominates(const Graph& g,uint32_t dom,uint32_t before) {
  std::vector<uint32_t> todo{before};std::set<uint32_t> seen;bool hit=false;
  while(!todo.empty()) {
    const auto node=todo.back();todo.pop_back();if(node==dom){hit=true;continue;}
    if(node==g.begin)return false;
    if(!seen.insert(node).second)continue;
    bool pred=false;
    for(const auto& [at,d]:g.instructions) {
      if(!(d.jump&&d.target==node)&&!(!d.terminal&&(!d.jump||d.conditional)&&d.next==node))continue;
      pred=true;todo.push_back(at);
    }
    if(!pred)return false;
  }
  return hit;
}
struct Pair {Arm native,ptx;uint32_t site=0,field=0,query_slot=0,query_field=0,query_call=0;unsigned self=0;};
inline bool ReadPair(const View& v,const Graph& g,const Ins& t,uint32_t dispatch,Pair& p) {
  if(!Test(&t,0))return false;
  const auto* q=g.Previous(t.rva);
  if(!Indirect(q,dispatch)||!mfggate::LinearEdge(g,q,&t))return false;
  const auto* j=g.Next(&t);if(!j||j->op!=0x74||j->next-j->rva!=2||!mfggate::LinearEdge(g,&t,j))return false;
  p.site=j->rva;p.query_call=q->rva;const auto ptx_begin=j->target;
  auto* i=g.Next(j);const auto sub=[](const Ins* x){return x&&x->op==0x83&&!x->memory&&x->width==32&&x->reg==5&&x->rm==0&&x->immediate==1;};
  if(!sub(i))return false;
  auto* j1=g.Next(i);if(!j1||j1->op!=0x74)return false;
  i=g.Next(j1);if(!sub(i))return false;
  auto* j2=g.Next(i);if(!j2||j2->op!=0x74||j2->target!=j1->target)return false;
  i=g.Next(j2);if(!i||i->op!=0x83||i->memory||i->width!=32||i->reg!=7||i->rm!=0||i->immediate!=1)return false;
  auto* j3=g.Next(i);if(!j3||j3->op!=0x75||j3->target!=ptx_begin||j3->next!=j1->target)return false;
  if(!ReadArm(v,g,j1->target,p.native)||!ReadArm(v,g,ptx_begin,p.ptx)||
      p.native.join!=p.ptx.join||p.native.allocator!=p.ptx.allocator||p.native.allocation!=p.ptx.allocation)return false;
  const auto* store=g.At(p.ptx.join);
  if(!store||store->op!=0x89||!store->memory||store->rip||store->sib||store->reg!=1||store->width!=64||
      store->displacement<=0||store->displacement>4096||!mfggate::HasThisAlias(g,store->rm,store->rva))return false;
  p.field=static_cast<uint32_t>(store->displacement);p.self=store->rm;
  const auto* method=Definition(g,q->rva,0);
  if(!method||!Mem(method,0x8b,0,0,method->displacement)||method->displacement<=0||method->displacement>128||method->displacement%8)return false;
  const auto* vt=Definition(g,method->rva,0);
  const auto* object=Definition(g,q->rva,1);
  if(!Mem(vt,0x8b,0,1,0)||!object||!Mem(object,0x8b,1,p.self,object->displacement)||
      object->displacement<=0||object->displacement>4096||!mfggate::HasThisAlias(g,p.self,object->rva)||
      Definition(g,vt->rva,1)!=object)return false;
  p.query_slot=static_cast<uint32_t>(method->displacement);p.query_field=static_cast<uint32_t>(object->displacement);
  // Reject entry into the comparison chain from an unrelated predecessor.
  for(const auto& [at,d]:g.instructions)
    if(d.jump&&d.target>j->next&&d.target<j1->target&&(at<j->next||at>=j1->target))return false;
  return true;
}

struct Mask {uint32_t left=0,right=0;unsigned bytes=0;};
// Compare decoded instruction graphs. Rel32/rel8 are normalized to their
// actual target (or equal within-function offset); all other bytes must agree
// except explicitly proven argument operands. This is pair equivalence within
// the CURRENT image, never a fingerprint of a previously accepted DLL.
inline bool Equivalent(const View& v,const Graph& a,const Graph& b,std::span<const Mask> masks) {
  if(a.end-a.begin!=b.end-b.begin||a.instructions.size()!=b.instructions.size())return false;
  for(const auto& entry:a.instructions) {
    // Clang 14 cannot capture structured bindings in the local lambda.
    // Ordinary const references preserve the same objects and lifetime.
    const auto& at=entry.first;
    const auto& x=entry.second;
    const auto* y=b.At(b.begin+(at-a.begin));
    if(!y||y->next-b.begin!=x.next-a.begin)return false;
    std::vector<unsigned char> xb(v.image.begin()+at,v.image.begin()+x.next);
    std::vector<unsigned char> yb(v.image.begin()+y->rva,v.image.begin()+y->next);
    const auto clear=[&](uint32_t xp,uint32_t yp,unsigned n){
      if(xp<at||yp<y->rva||xp+n>x.next||yp+n>y->next||xp-at!=yp-y->rva)return false;
      std::fill_n(xb.begin()+(xp-at),n,0);std::fill_n(yb.begin()+(yp-y->rva),n,0);return true;
    };
    bool masked=false;
    for(const auto& m:masks) if(m.left>=at&&m.left<x.next) {
      if(!clear(m.left,m.right,m.bytes))return false;
      masked=true;
    }
    if(x.jump||x.op==0xe8||x.rip) {
      if(!(x.jump==y->jump&&x.op==y->op&&x.rip==y->rip))return false;
      if(!masked) {
        const bool internal=x.jump&&x.target>=a.begin&&x.target<a.end&&y->target>=b.begin&&y->target<b.end;
        if(internal?x.target-a.begin!=y->target-b.begin:x.target!=y->target)return false;
        const unsigned n=x.jump&&(x.op==0xeb||(x.op>=0x70&&x.op<=0x7f))?1:4;
        // This contract uses RIP operands without a following immediate.
        if(x.rip&&(x.op!=0x8d&&x.op!=0x8b&&x.op!=0x89&&x.op!=0xff))return false;
        if(!clear(x.next-n,y->next-n,n))return false;
      }
    }
    if(xb!=yb)return false;
  }
  return true;
}
inline bool Constructor(const View& v,const Graph& g,std::string_view type,uint32_t& table,uint32_t& lea,StructuralPlan& p) {
  unsigned count=0;
  for(const auto& [at,i]:g.instructions) {
    if(i.op!=0x8d||!i.rip||i.width!=64||i.reg!=0||!i.target)continue;
    const auto* store=g.Next(&i);
    if(!store||!Mem(store,0x89,0,store->rm,0)||!mfggate::LinearEdge(g,&i,store)||
        !mfggate::HasThisAlias(g,store->rm,at)||!TableNamed(v,i.target,type,&p))continue;
    table=i.target;lea=at;if(++count>1)return false;
  }
  return count==1;
}
// The only other differing vtable slot is a name observer. Prove pair
// equivalence with the two literal lengths/addresses, rather than silently
// trusting arbitrary code behind slot 1.
inline bool NameObserver(const View& v,uint32_t left,uint32_t right,StructuralPlan& p) {
  if(left==right)return true;
  Graph a,b;if(!Build(v,left,a)||!Build(v,right,b))return false;
  std::vector<Mask> masks;
  for(const auto& [at,x]:a.instructions) {
    if(x.op!=0x8d||!x.rip||x.reg!=2||x.width!=64)continue;
    const auto* y=b.At(b.begin+(at-a.begin));
    if(!y||y->op!=0x8d||!y->rip||y->reg!=2||y->width!=64)continue;
    const auto ls=v.String(x.target),rs=v.String(y->target);
    if(ls!="Flatten_dlfg_bilerp"||rs!="Flatten_dlfg_bilerp_89_PTX")continue;
    const auto* al=a.Next(&x);const auto* bl=b.Next(y);
    if(!Mem(al,0x8d,8,0,static_cast<int64_t>(ls.size()),32)||
        !Mem(bl,0x8d,8,0,static_cast<int64_t>(rs.size()),32)||
        !Zero(Definition(a,al->rva,0),0)||!Zero(Definition(b,bl->rva,0),0)||
        al->mod!=1||bl->mod!=1)return false;
    masks.push_back({x.next-4,y->next-4,4});masks.push_back({al->next-1,bl->next-1,1});
    if(!AddWitness(v,p,x.target,ls.size()+1)||!AddWitness(v,p,y->target,rs.size()+1))return false;
  }
  return masks.size()==2&&Equivalent(v,a,b,masks)&&Remember(v,p,a)&&Remember(v,p,b);
}
struct Load {PayloadReference payload;uint32_t pointer_operand=0,length_operand=0,call=0,field=0;};
inline bool Loader(const View& v,const Graph& g,std::vector<Load>& loads,uint32_t& helper) {
  loads.clear();helper=0;
  // Flat loader is a complete straight-line .pdata function. Every tuple is
  // built from RDX/R8/R9 and RCX=this, then stored into an object handle field.
  for(const auto& [at,i]:g.instructions) {
    if(i.jump||(i.terminal&&i.op!=0xc3))return false;
    if(i.op!=0x8d||!i.rip||i.width!=64||i.reg!=2)continue;
    if(!v.InSection(i.target,16,false)||v.U32(i.target)!=fatbin::kMagic)continue;
    const auto bytes64=v.U64(i.target+8)+fatbin::kHeaderBytes;
    if(bytes64>fatbin::kMaxFatbinBytes||!v.InSection(i.target,static_cast<size_t>(bytes64),false))return false;
    Load load;load.payload.rva=i.target;load.payload.bytes=static_cast<uint32_t>(bytes64);load.pointer_operand=i.next-4;
    uint32_t length=0;std::string name;bool self=false;unsigned steps=0;
    // R9 normally precedes RDX, by field loads/stack stores only. Walk back to
    // its defining LEA without crossing a clobber or call.
    for(auto* b=g.Previous(at);b&&steps++<24;b=g.Previous(b->rva)) {
      if(b->call||b->jump)return false;
      if(b->op==0x8d&&b->rip&&b->width==64&&b->reg==9){name=std::string(v.String(b->target,256));break;}
      if(mfggate::WritesRegister(*b,9))return false;
    }
    if(name.empty())return false;
    steps=0;const Ins* c=nullptr;
    for(auto* n=g.Next(&i);n&&steps++<24;n=g.Next(n)) {
      if(n->call) {c=n;break;}
      if(n->jump||n->terminal)return false;
      unsigned r=0;uint32_t value=0;
      if(mfggate::IsImmMov(n,r,value)&&r==8){if(length)return false;length=value;load.length_operand=n->immediate_rva;continue;}
      if(mfggate::IsMovReg(n,1,3)&&mfggate::HasThisAlias(g,3,at)){self=true;continue;}
      if(mfggate::WritesRegister(*n,2)||mfggate::WritesRegister(*n,8)||mfggate::WritesRegister(*n,9)||mfggate::WritesRegister(*n,1))return false;
    }
    if(!c||c->op!=0xe8||!c->target||!self||length!=load.payload.bytes||
        !structural::PtxHasSourceEntry(v.image.subspan(load.payload.rva,load.payload.bytes),name))return false;
    if(helper&&helper!=c->target)return false;
    helper=c->target;
    auto* store=g.Next(c);unsigned stores=0;
    while(store&&!Mem(store,0x89,0,3,store->displacement)&&stores++<16) {
      if(store->call||store->jump||store->terminal||mfggate::WritesRegister(*store,0))return false;
      store=g.Next(store);
    }
    if(!store||!Mem(store,0x89,0,3,store->displacement)||store->displacement<0||store->displacement>1024*1024)return false;
    load.payload.entry_name=std::move(name);load.call=c->rva;load.field=static_cast<uint32_t>(store->displacement);
    for(const auto& old:loads)if(old.field==load.field)return false;
    loads.push_back(std::move(load));if(loads.size()>256)return false;
  }
  if(!helper||loads.empty()||!v.Function(helper).first)return false;
  size_t calls=0;for(const auto& [at,i]:g.instructions)if(i.call&&i.op==0xe8&&i.target==helper)++calls;
  return calls==loads.size();
}

// Verify the common helper preserves payload, byte length and entry name into
// the backend virtual load call. Values are symbolic register origins; stack
// dimensions are irrelevant, but ANY other definition kills an origin.
inline bool Helper(const View& v,const Graph& g,uint32_t dispatch) {
  enum Value {unknown,this_ptr,payload,length,name,backend,vtable,load_method};
  std::array<Value,16> regs{};regs[1]=this_ptr;regs[2]=payload;regs[8]=length;regs[9]=name;
  unsigned calls=0;
  for(const auto& [at,i]:g.instructions) {
    (void)at;if(i.jump)return false;
    if(i.call) {
      if(!Indirect(&i,dispatch)||regs[0]!=load_method||regs[1]!=backend||regs[2]!=payload||regs[8]!=length||regs[9]!=name)return false;
      if(++calls!=1)return false;
      for(unsigned r:{0u,1u,2u,8u,9u,10u,11u})regs[r]=unknown;
      continue;
    }
    bool handled=false;
    if((i.op==0x8b||i.op==0x89)&&!i.memory) {
      const auto dst=i.op==0x8b?i.reg:i.rm,src=i.op==0x8b?i.rm:i.reg;
      regs[dst]=(i.width==64||(i.width==32&&regs[src]==length))?regs[src]:unknown;handled=true;
    } else if(i.op==0x8b&&i.memory&&!i.sib&&!i.rip&&i.width==64) {
      const auto b=regs[i.rm];Value value=unknown;
      if(b==this_ptr&&i.displacement>0&&i.displacement<4096)value=backend;
      if(b==backend&&i.displacement==0)value=vtable;
      if(b==vtable&&i.displacement>0&&i.displacement<=128&&(i.displacement%8)==0)value=load_method;
      regs[i.reg]=value;handled=true;
    }
    if(!handled)for(unsigned r=0;r<16;++r)if(mfggate::WritesRegister(i,r))regs[r]=unknown;
  }
  (void)v;return calls==1;
}

// Actual shared Init virtual method: R9b gates the loader (slot 4), and the
// wrapper must explicitly pass zero. Decoded instructions are required at all
// operands; this is not a raw byte scan.
inline bool Init(const View& v,const Graph& g,uint32_t dispatch) {
  for(const auto& [at,i]:g.instructions) {
    if(i.op!=0x0f||i.op2!=0xb6||i.memory||i.rm!=9)continue;
    const unsigned flag=i.reg;
    for(const auto& [ta,t]:g.instructions) {
      if(ta<=at||t.op!=0x84||t.memory||t.reg!=flag||t.rm!=flag)continue;
      bool intact=true;for(const auto& [a,d]:g.instructions)if(a>at&&a<ta&&mfggate::WritesRegister(d,flag))intact=false;
      auto* j=g.Next(&t);if(!intact||!j||j->op!=0x75)continue;
      auto* vt=g.Next(j);if(!vt||!Mem(vt,0x8b,0,vt->rm,0)||!mfggate::HasThisAlias(g,vt->rm,vt->rva))continue;
      auto* self=g.Next(vt);auto* method=g.Next(self);auto* call=g.Next(method);
      if(!mfggate::IsMovReg(self,1,vt->rm)||!Mem(method,0x8b,0,0,32)||!Indirect(call,dispatch)||j->target<=call->rva)continue;
      if(!mfggate::LinearSlice(g,{vt,self,method,call}))continue;
      // No unmodelled bypass may enter the body; only the flag JNE skips it.
      bool bypass=false;for(const auto& [a,d]:g.instructions)if(d.jump&&a!=j->rva&&d.target>j->rva&&d.target<=call->rva)bypass=true;
      if(!bypass)return true;
    }
  }
  (void)v;return false;
}
inline bool Consumer(const Graph& g,const Pair& pair,uint32_t dispatch) {
  unsigned hits=0;
  for(const auto& [at,i]:g.instructions) {
    if(!Mem(&i,0x8b,1,pair.self,pair.field)||at<=pair.ptx.join||
        !mfggate::HasThisAlias(g,pair.self,at)||!Dominates(g,pair.ptx.join,at))continue;
    auto* n=g.Next(&i);if(!Mem(n,0x8b,0,1,0))continue;
    bool zero=false;unsigned steps=0;
    for(n=g.Next(n);n&&steps++<12;n=g.Next(n)) {
      if(Zero(n,9)){zero=true;continue;}
      if(Mem(n,0x8b,0,0,16)) {
        if(zero&&Indirect(g.Next(n),dispatch))++hits;
        break;
      }
      if(n->call||n->jump||mfggate::WritesRegister(*n,0)||mfggate::WritesRegister(*n,1)||mfggate::WritesRegister(*n,9))break;
    }
  }
  return hits==1;
}

inline bool Find(const View& v,StructuralPlan& p,std::string& why) {
  const auto fail=[&](const char* reason){why=reason;return false;};
  const auto wrappers=Tables(v,".?AVDLFGNetworkWrapperFlat@@");
  if(wrappers.size()!=1)return fail("endpoint flat: unique primary wrapper RTTI/vtable not found");
  const auto table=wrappers[0];
  if(!TableNamed(v,table,".?AVDLFGNetworkWrapperFlat@@",&p))return false;
  Graph factory;const auto factory_rva=v.PointerRva(v.U64(table+8));
  if(!Build(v,factory_rva,factory)||!Remember(v,p,factory))return fail("endpoint flat: incomplete wrapper Init CFG");
  const auto dispatch=mfggate::DispatchSlot(v);if(!dispatch)return fail("endpoint flat: missing CFG dispatch binding");
  if(!AddWitness(v,p,dispatch,8))return false;
  std::array<unsigned,2> found{};std::array<Pair,2> pairs{};uint32_t shared_helper=0;
  for(const auto& [at,i]:factory.instructions) {
    (void)at;Pair pair;if(!ReadPair(v,factory,i,dispatch,pair))continue;
    Graph native,ptx;
    if(!Build(v,pair.native.constructor,native)||!Build(v,pair.ptx.constructor,ptx))continue;
    uint32_t nt=0,pt=0,nlea=0,plea=0;size_t role=2;
    for(size_t r=0;r<2;++r) {
      const std::string suffix=r==0?"@dx@@":"@vk@@";
      if(Constructor(v,native,".?AVFlatten_dlfg_bilerp"+suffix,nt,nlea,p)&&
          Constructor(v,ptx,".?AVFlatten_dlfg_bilerp_89_PTX"+suffix,pt,plea,p)){role=r;break;}
    }
    if(role==2)continue;
    if(++found[role]!=1)return fail("endpoint flat: ambiguous duplicate API selector");
    const std::array<Mask,1> ctor_mask{{{native.At(nlea)->next-4,ptx.At(plea)->next-4,4}}};
    if(!Equivalent(v,native,ptx,ctor_mask))return fail("endpoint flat: native/PTX constructor semantics differ");
    // Same destruction, initialization and shape methods, not merely same type
    // name. Slot 1 is only the name observer; slot 4 the validated loader.
    // The wrapper consumes slots through 14; corresponding render/evaluate,
    // shape, destruction and resource methods must all be the SAME functions.
    if(!v.InSection(nt,15*8,false)||!v.InSection(pt,15*8,false))return fail("endpoint flat: truncated network vtable");
    for(unsigned slot=0;slot<15;++slot) {
      if(!v.InSection(v.PointerRva(v.U64(nt+slot*8)),1,true)||!v.InSection(v.PointerRva(v.U64(pt+slot*8)),1,true))
        return fail("endpoint flat: invalid network method");
      if(slot!=1&&slot!=4&&v.U64(nt+slot*8)!=v.U64(pt+slot*8))return fail("endpoint flat: object lifetime/evaluate/Init ABI mismatch");
    }
    if(!AddWitness(v,p,nt,15*8)||!AddWitness(v,p,pt,15*8))return false;
    if(!NameObserver(v,v.PointerRva(v.U64(nt+8)),v.PointerRva(v.U64(pt+8)),p))
      return fail("endpoint flat: name observer differs beyond class label");
    Graph nl,pl,init;
    if(!Build(v,v.PointerRva(v.U64(nt+32)),nl)||!Build(v,v.PointerRva(v.U64(pt+32)),pl)||nl.begin==pl.begin||
        !Build(v,v.PointerRva(v.U64(pt+16)),init)||!Init(v,init,dispatch)||!Consumer(factory,pair,dispatch))
      return fail("endpoint flat: unproven wrapper-to-Init-to-loader call");
    std::vector<Load> loads;uint32_t helper=0;
    if(!Loader(v,pl,loads,helper))return fail("endpoint flat: incomplete PTX payload/name/length coverage");
    if(shared_helper&&shared_helper!=helper)return fail("endpoint flat: inconsistent API loading helper");
    shared_helper=helper;
    Graph helper_graph;if(!Build(v,helper,helper_graph)||!Helper(v,helper_graph,dispatch))return fail("endpoint flat: loading helper does not forward payload contract");
    std::vector<Mask> masks;
    for(const auto& l:loads) {
      const uint32_t lo=nl.begin+(l.pointer_operand-pl.begin),ll=nl.begin+(l.length_operand-pl.begin);
      // Operand locations are derived from corresponding decoded instructions.
      const Ins* np=nullptr;const Ins* sz=nullptr;
      for(const auto& [a,d]:nl.instructions){(void)a;if(d.op==0x8d&&d.rip&&d.reg==2&&d.next-4==lo)np=&d;if(d.immediate_rva==ll)sz=&d;}
      unsigned reg=0;uint32_t bytes=0;
      if(!np||!mfggate::IsImmMov(sz,reg,bytes)||reg!=8||bytes==0||bytes>fatbin::kMaxFatbinBytes||!v.InSection(np->target,bytes,false))
        return fail("endpoint flat: native loader tuple relation invalid");
      masks.push_back({lo,l.pointer_operand,4});masks.push_back({ll,l.length_operand,4});
      p.payloads.push_back(l.payload);
    }
    if(!Equivalent(v,nl,pl,masks))return fail("endpoint flat: native/PTX loaders differ beyond payload arguments");
    if(!Remember(v,p,native)||!Remember(v,p,ptx)||!Remember(v,p,nl)||!Remember(v,p,pl)||!Remember(v,p,init)||!Remember(v,p,helper_graph))return false;
    p.selectors[role]={pair.site,{v.image[pair.site],v.image[pair.site+1]},{0xeb,v.image[pair.site+1]}};
    p.loaders[role]={pt+32,pl.begin};p.network_counts[role]=loads.size();pairs[role]=pair;
  }
  if(found[0]!=1||found[1]!=1)return fail("endpoint flat: complete DX/Vulkan factory pair not proven");
  if(p.selectors[0].rva==p.selectors[1].rva||p.loaders[0].rva==p.loaders[1].rva)return fail("endpoint flat: overlapping roles");
  if(pairs[0].self!=pairs[1].self||pairs[0].field==pairs[1].field||
      pairs[0].query_slot!=pairs[1].query_slot||pairs[0].query_field!=pairs[1].query_field)
    return fail("endpoint flat: inconsistent backend query/ownership");
  unsigned api_splits=0;
  for(const auto& [at,d]:factory.instructions) {
    (void)at;
    if(d.conditional&&((d.next==pairs[0].query_call&&d.target==pairs[1].query_call)||
        (d.next==pairs[1].query_call&&d.target==pairs[0].query_call)))++api_splits;
  }
  if(api_splits!=1)return fail("endpoint flat: DX/Vulkan dispatch is not a unique API branch");
  // Wrapper destruction must consume both corresponding owned pointers and
  // invoke their deleting destructor; the native and PTX destructors above
  // are identical. No new allocation owner is introduced by this selection.
  Graph destructor;if(!Build(v,v.PointerRva(v.U64(table)),destructor))return fail("endpoint flat: wrapper destructor CFG unavailable");
  for(const auto& pair:pairs) {
    unsigned hits=0;
    for(const auto& [at,d]:destructor.instructions)
      if(d.op==0x8b&&d.width==64&&d.reg==1&&d.memory&&!d.rip&&!d.sib&&d.displacement==pair.field&&
          mfggate::HasThisAlias(destructor,d.rm,at)&&Test(destructor.Next(&d),1,64))++hits;
    if(hits!=1)return fail("endpoint flat: wrapper does not own selected network field");
  }
  unsigned deletes=0;
  for(const auto& [at,d]:destructor.instructions) {
    (void)at;if(!Mem(&d,0x8b,0,1,0))continue;
    const auto* arg=destructor.Next(&d);const auto* slot=destructor.Next(arg);const auto* call=destructor.Next(slot);
    if(Imm(arg,2,1)&&Mem(slot,0x8b,0,0,0)&&Indirect(call,dispatch))++deletes;
  }
  if(deletes!=1||!Remember(v,p,destructor))return fail("endpoint flat: deleting-destructor contract unavailable");
  p.family=Family::kLegacyFlat;why.clear();return true;
}
} // namespace flat

inline bool FindEndpointPlan(std::span<const unsigned char> image,std::span<const ImageRange> ranges,
                             StructuralPlan& output,std::string& why) {
  output={};
  StructuralPlan modern;std::string modern_reason;
  const bool have_modern=FindStructuralPlan(image,ranges,modern,modern_reason);
  structural::ImageView v{image,ranges,{}};
  if(!v.ReadFunctions()){why=modern_reason;return false;}
  StructuralPlan legacy;std::string legacy_reason;
  const bool have_legacy=flat::Find(v,legacy,legacy_reason);
  if(have_modern&&have_legacy){why="endpoint: ambiguous modern and flat families";return false;}
  if(have_modern){output=std::move(modern);why.clear();return true;}
  if(have_legacy){output=std::move(legacy);why.clear();return true;}
  why=modern_reason+"; "+legacy_reason;return false;
}
inline bool WitnessesMatch(std::span<const unsigned char> image,const StructuralPlan& p) {
  for(const auto& w:p.witnesses)if(w.rva>image.size()||w.bytes.size()>image.size()-w.rva||
      !std::equal(w.bytes.begin(),w.bytes.end(),image.begin()+w.rva))return false;
  for(const auto& s:p.selectors)if(s.rva>image.size()||s.before.size()>image.size()-s.rva||
      !std::equal(s.before.begin(),s.before.end(),image.begin()+s.rva))return false;
  return true;
}
} // namespace mfgunlock::endpoint
