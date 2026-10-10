// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/endpoint_backend.hpp"
#include "../tools/mapped_provider_image.hpp"
#include <iostream>
#include <random>
#include <stdexcept>

namespace e=mfgunlock::endpoint;
namespace f=mfgunlock::fatbin;
using Bytes=std::vector<unsigned char>;
size_t checks=0;
void Check(bool ok,const char* reason) { ++checks; if(!ok) throw std::runtime_error(reason); }
template<class T> void Put(Bytes& b,size_t at,T value) { std::memcpy(b.data()+at,&value,sizeof(value)); }
void Code(Bytes& b,size_t at,std::initializer_list<unsigned char> code) {
  std::copy(code.begin(),code.end(),b.begin()+at);
}
void Relative(Bytes& b,size_t at,size_t disp,size_t length,size_t dest) {
  Put<int32_t>(b,at+disp,static_cast<int32_t>(dest)-static_cast<int32_t>(at+length));
}
void String(Bytes& b,size_t at,const std::string& value) {std::copy(value.begin(),value.end(),b.begin()+at);}

// Independent miniature contract. No DLL, original RVAs, source hash or fixed
// 25/14 network counts are used. Each loader has one PTX payload.
struct Fixture {
  Bytes b=Bytes(0xa000);
  std::vector<e::ImageRange> ranges{{0x1000,0x3000,true},{0x4000,0x6000,false}};
  std::vector<std::pair<uint32_t,uint32_t>> functions;
  std::array<size_t,2> selectors{},tuples{};
  void Function(size_t begin,size_t end) { functions.emplace_back(begin,end); }
  void Constructor(size_t at,size_t table,size_t rtti,std::string name,size_t loader) {
    Code(b,at,{0x48,0x8d,0x05,0,0,0,0,0x48,0x89,0x03,0xc3});
    Relative(b,at,3,7,table);Function(at,at+11);
    Put<uint64_t>(b,table-8,reinterpret_cast<uintptr_t>(b.data())+rtti);
    Put<uint64_t>(b,table+32,reinterpret_cast<uintptr_t>(b.data())+loader);
    Put<uint32_t>(b,rtti,1);Put<uint32_t>(b,rtti+12,rtti+32);Put<uint32_t>(b,rtti+20,rtti);
    String(b,rtti+48,name);
  }
  // Three independent compiler spellings of the allocation/null arm.
  size_t Arm(size_t at,size_t ctor,unsigned reg,unsigned mode) {
    const size_t start=at;
    Code(b,at,{0xe8,0,0,0,0,0x48,0x89,0x44,0x24,0x20});
    Relative(b,at,1,5,0x1200); at+=10;
    if(mode==1) { Code(b,at,{0x33,0xf6});at+=2; }
    const size_t test=at;
    Code(b,at,{0x48,0x85,0xc0,0x74,0,0x48,0x8b,0xc8,0xe8,0,0,0,0});
    Relative(b,at+8,1,5,ctor);at+=13;
    Code(b,at,{static_cast<unsigned char>(reg>=8?0x4c:0x48),0x8b,
               static_cast<unsigned char>(0xc0|((reg&7)<<3))});at+=3;
    if(mode==2) b[test+4]=static_cast<unsigned char>(at-test-5);
    else {
      const size_t skip=at;Code(b,at,{0xeb,0});at+=2;
      b[test+4]=static_cast<unsigned char>(at-test-5);
      if(mode==1) { Code(b,at,{static_cast<unsigned char>(reg>=8?0x4c:0x48),0x8b,
                              static_cast<unsigned char>(0xc6|((reg&7)<<3))});at+=3; }
      else {
        if(reg>=8)b[at++]=0x45;
        Code(b,at,{0x33,static_cast<unsigned char>(0xc0|((reg&7)<<3)|(reg&7))});at+=2;
      }
      b[skip+1]=static_cast<unsigned char>(at-skip-2);
    }
    return at-start;
  }
  explicit Fixture(unsigned slot=0x40,unsigned mode=0,unsigned reg=7) {
    Code(b,0,{'M','Z'});Put<uint32_t>(b,0x3c,0x80);Put<uint32_t>(b,0x80,0x4550);
    Put<uint16_t>(b,0x84,0x8664);Put<uint16_t>(b,0x94,240);Put<uint16_t>(b,0x98,0x20b);
    Put<uint64_t>(b,0x98+24,0x180000000ull);Put<uint32_t>(b,0x98+56,b.size());
    Put<uint32_t>(b,0x98+60,0x400);Put<uint32_t>(b,0x98+108,16);
    Code(b,0x1200,{0xc3});Function(0x1200,0x1201);
    Code(b,0x1210,{0xc3});Function(0x1210,0x1211);
    for (size_t role=0;role<2;++role) {
      const size_t table=0x5000+role*0x1000,ctor=0x1300+role*0x100,loader=0x1800+role*0x100;
      const size_t payload=0x7000+role*0x800,name=payload+0x600;
      const std::string type=".?AVFlatten_EndpointDL"+std::to_string(role+1)+"Net_89";
      Constructor(ctor,table,table+0x100,type+"_PTX@@",loader);
      Constructor(ctor+0x20,table+0x300,table+0x400,type+"@@",0x1210);
      Constructor(ctor+0x40,table+0x600,table+0x700,
          ".?AVFlatten_EndpointDL"+std::to_string(role+1)+"Net@@",0x1210);
      const std::string ptx=".version 8.2\n.target sm_89\n.address_size 64\n.visible .entry main_kernel() {ret;}\n";
      const size_t stored=(ptx.size()+1+7)&~size_t{7};
      const size_t total=16+64+stored;
      Put<uint32_t>(b,payload,f::kMagic);Put<uint16_t>(b,payload+4,1);Put<uint16_t>(b,payload+6,16);
      Put<uint64_t>(b,payload+8,total-16);Put<uint16_t>(b,payload+16,1);
      Put<uint16_t>(b,payload+18,0x101);Put<uint32_t>(b,payload+20,64);
      Put<uint64_t>(b,payload+24,stored);Put<uint32_t>(b,payload+44,89);
      Put<uint64_t>(b,payload+56,0x41);String(b,payload+80,ptx);String(b,name,"main_kernel");
      Code(b,loader,{0x4c,0x8d,0x0d,0,0,0,0,0x41,0xb8,0,0,0,0,
        0x48,0x8d,0x15,0,0,0,0,0x48,0x8b,0xcb,0xe8,0,0,0,0,0xc3});
      Relative(b,loader,3,7,name);Put<uint32_t>(b,loader+9,total);
      Relative(b,loader+13,3,7,payload);Relative(b,loader+23,1,5,0x1210);
      Function(loader,loader+29);tuples[role]=loader;
      const size_t start=0x2000+role*0x100,cmp=start+18;
      Code(b,start,{0x48,0x8b,0x01,0x48,0x8b,0x40,static_cast<unsigned char>(slot),
         0xff,0x15,0,0,0,0,0xb9,0,0x10,0,0,0x83,0xf8,0x59,0x7e,0});
      Relative(b,start+7,2,6,0x4800);
      selectors[role]=cmp+3;
      size_t at=cmp+5;
      at+=Arm(at,ctor,reg,mode);const size_t j1=at;at+=2;
      const size_t native=at;Code(b,at,{0x7c,0});at+=2;
      at+=Arm(at,ctor+0x20,reg,mode);const size_t j2=at;at+=2;
      const size_t lower=at;at+=Arm(at,ctor+0x40,reg,mode);
      Code(b,j1,{0xeb,static_cast<unsigned char>(at-j1-2)});
      Code(b,j2,{0xeb,static_cast<unsigned char>(at-j2-2)});
      b[cmp+4]=static_cast<unsigned char>(native-cmp-5);
      b[native+1]=static_cast<unsigned char>(lower-native-2);
      while(at<start+0x100)b[at++]=0x90;
    }
    b[0x2200]=0xc3;Function(0x2000,0x2201);
    std::sort(functions.begin(),functions.end());
    Put<uint32_t>(b,0x98+136,0x4000);Put<uint32_t>(b,0x98+140,functions.size()*12);
    for(size_t i=0;i<functions.size();++i) {
      Put<uint32_t>(b,0x4000+i*12,functions[i].first);Put<uint32_t>(b,0x4004+i*12,functions[i].second);
    }
  }
};

int main() {
  try {
    Fixture x; e::StructuralPlan p;std::string why;
    const bool found=e::FindStructuralPlan(x.b,x.ranges,p,why); Check(found,why.c_str());
    Check(p.payloads.size()==2 && p.network_counts[0]==1 && p.network_counts[1]==1,"derived network counts");
    for(size_t r=0;r<2;++r)Check(p.selectors[r].rva==x.selectors[r],"derived selector RVA");
    for(const auto slot : {0x40u,0x48u,0x50u}) {
      for(const auto mode : {0u,1u,2u}) {
        for(const auto reg : {3u,5u,6u,7u,12u,13u,14u,15u}) {
          Fixture variant(slot,mode,reg);
          Check(e::FindStructuralPlan(variant.b,variant.ranges,p,why),why.c_str());
          Check(p.payloads.size()==2,"compiler variant binds both loaders");
        }
      }
    }
    for(const auto slot : {0u,0x41u,0x80u}) {
      Fixture variant(slot);
      Check(!e::FindStructuralPlan(variant.b,variant.ranges,p,why),"invalid virtual slot refused");
    }
    const auto original=x.b;
    for(const auto at : {x.selectors[0],x.tuples[0]+9,size_t{0x5100+20},size_t{0x5100+48}}) {
      x.b[at]^=1;
      Check(!e::FindStructuralPlan(x.b,x.ranges,p,why),"corrupted dispatch/length/RTTI rejected");
      Check(p.payloads.empty(),"failed match publishes nothing");
      std::copy(original.begin(),original.end(),x.b.begin());
    }
    // Reject a second plausible factory instead of choosing first occurrence.
    auto first=x.functions.back();
    x.functions.push_back(first);Put<uint32_t>(x.b,0x98+140,x.functions.size()*12);
    Put<uint32_t>(x.b,0x4000+(x.functions.size()-1)*12,first.first);
    Put<uint32_t>(x.b,0x4004+(x.functions.size()-1)*12,first.second);
    Check(!e::FindStructuralPlan(x.b,x.ranges,p,why),"overlapping functions rejected");
    std::copy(original.begin(),original.end(),x.b.begin());
    e::structural::ImageView view{x.b,x.ranges,{}};
    Code(x.b,0x3000,{0x8b,0x83,0xe8,0,0,0,0xc3});
    Check(e::structural::InstructionBytes(view,0x3000,0x3007)==6,"E8 inside displacement is not call");
    Check(e::structural::InstructionBytes(view,0x3000,0x3004)==0,"truncated instruction rejected");
    std::mt19937 random(0x751234);
    for(size_t i=0;i<1024;++i) {
      x.b=original; // refresh relocated pointers after any vector copy
      const size_t at=0x2000+(random()%513);x.b[at]^=static_cast<unsigned char>((random()%255)+1);
      const auto before=x.b;
      (void)e::FindStructuralPlan(x.b,x.ranges,p,why);
      Check(before==x.b,"scanner is read only");
    }
    // Relocation validation is transactional, including overlapping targets.
    x.b=original;
    Put<uint32_t>(x.b,0x98+152,0x9000);Put<uint32_t>(x.b,0x98+156,12);
    Put<uint32_t>(x.b,0x9000,0x5000);Put<uint32_t>(x.b,0x9004,12);
    Put<uint16_t>(x.b,0x9008,0xa000);Put<uint16_t>(x.b,0x900a,0xa000);
    const auto duplicate=x.b;bool failed=false;
    try {mfgunlock::validation::RelocateMappedProvider(x.b);}catch(const std::exception&){failed=true;}
    Check(failed && x.b==duplicate,"invalid relocation plan never partly applies");
    std::cout<<"endpoint structural checks="<<checks<<" PASS\n";
  }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
