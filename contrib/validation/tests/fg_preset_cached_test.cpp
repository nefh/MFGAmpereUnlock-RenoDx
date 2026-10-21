// SPDX-License-Identifier: MIT
// Independent synthetic code/data fixtures; no vendor DLL or version table.
#include "../../../src/addons/mfgunlock/fg_preset_cached.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace cg = mfgunlock::fgpreset::cached;
namespace mg = mfgunlock::mfggate;
namespace {
unsigned checks = 0;
void Check(bool result, const char* reason) {
  ++checks;
  if (!result) throw std::runtime_error(reason);
}
struct Fixture {
  std::vector<unsigned char> image = std::vector<unsigned char>(0x5000, 0xcc);
  std::vector<mg::Range> ranges{{0x100,0x2800,true},{0x3000,0x2000,false}};
  std::vector<std::pair<uint32_t,uint32_t>> functions;
  uint32_t reader, getter, object;
  std::vector<unsigned char> code;
  void Bytes(std::initializer_list<unsigned char> b) {
    // Append bytewise to avoid GCC 12 range-insert diagnostics in this fixture.
    for (const unsigned char byte : b) code.push_back(byte);
  }
  void Relative(size_t field, uint32_t next, uint32_t target) {
    const auto delta = static_cast<int32_t>(static_cast<int64_t>(target) - next);
    std::memcpy(code.data()+field, &delta, 4);
  }
  void Epilogue(bool bad) {
    Bytes({0x48,0x8b,0x5c,0x24,0x38,0x48,0x83,0xc4,static_cast<unsigned char>(bad?0x18:0x20),0x5f,0xc3});
  }
  explicit Fixture(uint32_t shift=0, std::string_view defect={})
      : reader(0x100+shift), getter(0x700+shift), object(0x3000+shift) {
    Bytes({0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa});
    if(defect=="upper input") Bytes({0x48,0x8b,0xd9}); else Bytes({0x8b,0xd9});
    if(defect=="third input") Bytes({0x41,0x8b,0xc0});
    if(defect=="stack input") Bytes({0x8b,0x44,0x24,0x50});
    if(defect=="return write") Bytes({0x89,0x4c,0x24,0x28});
    if(defect=="unknown") Bytes({0xc5,0xf8,0x77});
    const auto call = static_cast<uint32_t>(code.size());
    Bytes({0xe8,0,0,0,0});Relative(call+1,reader+call+5,getter);
    if(defect=="two calls") {
      const auto second=static_cast<uint32_t>(code.size());
      Bytes({0xe8,0,0,0,0});Relative(second+1,reader+second+5,getter);
    }
    Bytes({0x8b,0x48,0x04,0x85,0xdb,0x0f,0x84,0,0,0,0});
    const auto failure_field = code.size()-4;
    const auto failure_next = reader+static_cast<uint32_t>(code.size());
    if(defect=="no output") Bytes({0x90,0x90,0x90});
    else if(defect=="wide output") Bytes({0x48,0x89,0x0f});
    else if(defect=="extra output") Bytes({0x89,0x4f,0x04});
    else if(defect=="native write") Bytes({0x40,0x89,0x08});
    else Bytes({0x40,0x89,0x0f});
    if(defect=="lost saved") Bytes({0x89,0x4c,0x24,0x38});
    Bytes({0xb0,static_cast<unsigned char>(defect=="not bool"?2:1)});
    Epilogue(defect=="unbalanced");
    Relative(failure_field,failure_next,reader+static_cast<uint32_t>(code.size()));
    Bytes({0x32,0xc0});Epilogue(false);
    std::copy(code.begin(),code.end(),image.begin()+reader);
    functions.emplace_back(reader,reader+static_cast<uint32_t>(code.size()));
    std::vector<unsigned char> get{0x48,0x8d,0x05,0,0,0,0,0xc3};
    const auto delta=static_cast<int32_t>(object-(getter+7));
    std::memcpy(get.data()+3,&delta,4);
    if(defect=="unknown getter") get.back()=0xcc;
    std::copy(get.begin(),get.end(),image.begin()+getter);
    functions.emplace_back(getter,getter+static_cast<uint32_t>(get.size()));
  }
  void StaticLocalAccessor() {
    std::copy(std::begin(cg::kLocalCacheAccessor),std::end(cg::kLocalCacheAccessor),image.begin()+getter);
    functions[1].second=getter+static_cast<uint32_t>(sizeof(cg::kLocalCacheAccessor));
    const uint32_t shift=reader-0x100, callback=0x1800+shift, destroy=0x1b00+shift;
    const auto rel=[&](uint32_t field,uint32_t next,uint32_t to) {
      const auto delta=static_cast<int32_t>(static_cast<int64_t>(to)-next);
      std::memcpy(image.data()+field,&delta,4);
    };
    std::map<uint32_t,uint32_t> targets{
      {0x04,object+0x58},{0x1f,object+0x48},{0x27,object},{0x33,object+0x48},
      {0x3f,object+0x48},{0x48,object},{0x4f,object+8},{0x5e,object+8},
      {0x66,object+0x10},{0x71,object+0x18},{0x8d,object+0x10},{0x97,object+0x20},
      {0x9f,object+0x30},{0xaa,object+0x38},{0xb5,object+0x40},{0xc0,object+0x60},
      {0xc8,object+8},{0xd8,object+0x20},{0xe5,callback},{0xf2,object+0x48},{0xfe,object}};
    uint32_t helper=0x1900+shift;
    for(uint32_t call:{0x3au,0x81u,0xdfu,0xecu,0xf9u}) {
      targets[call]=helper;
      image[helper]=0x31;image[helper+1]=0xc0;image[helper+2]=0xc3;
      functions.emplace_back(helper,helper+3);helper+=0x20;
    }
    for(const auto& fix:cg::kAccessorRelocations)
      rel(getter+fix.field,getter+fix.next,targets.at(fix.instruction));
    const unsigned char tail[]={0x48,0x8d,0x0d,0,0,0,0,0xe9,0,0,0,0};
    std::copy(std::begin(tail),std::end(tail),image.begin()+callback);
    rel(callback+3,callback+7,object+8);rel(callback+8,callback+12,destroy);
    image[destroy]=0xc3;functions.emplace_back(destroy,destroy+1);
    std::sort(functions.begin(),functions.end());
  }
  bool Resolves(cg::Evidence& evidence) const {return cg::Qualify(mg::View{image,ranges,functions},reader,evidence);}
};
void Reader() {
  for(uint32_t shift:{0u,0x400u,0x1000u}) {
    Fixture f(shift);cg::Evidence evidence;
    Check(f.Resolves(evidence),"relocated cached DWORD reader with AL return qualifies");
    Check(evidence.accessor==f.getter && evidence.functions.size()==2,"reader and getter evidence retained");
    const auto before=f.image;Check(f.Resolves(evidence)&&f.image==before,"qualifier is read-only and deterministic");
  }
  for(std::string_view defect:{"upper input","third input","stack input","return write","unknown",
      "two calls","no output","wide output","extra output","native write","lost saved","not bool",
      "unbalanced","unknown getter"}) {
    Fixture f(0,defect);cg::Evidence evidence;
    if(f.Resolves(evidence)) throw std::runtime_error("accepted malformed ABI: "+std::string(defect));
    ++checks;
  }
  Fixture f;cg::Evidence evidence;
  f.ranges[1].executable=true;
  Check(!f.Resolves(evidence),"cache cannot alias executable code");
  f.ranges[1].executable=false;
  --f.functions[0].second;
  Check(!f.Resolves(evidence),"truncated epilogue is not a complete function");
}
void Accessor() {
  for(uint32_t shift:{0u,0x100u,0x400u}) {
    Fixture f(shift);f.StaticLocalAccessor();cg::Evidence evidence;
    Check(f.Resolves(evidence),"native static-local accessor is location independent");
    Check(evidence.accessor==f.getter&&evidence.functions.size()==9,"all inspected native dependencies retained");
    const auto original=f.image;
    f.image[f.getter+0x36]^=4;
    Check(!f.Resolves(evidence),"two initialization-guard references must agree");
    f.image=original;f.image[f.getter+0x52]^=4;
    Check(!f.Resolves(evidence),"cache field reference cannot silently move");
    f.image=original;f.image[f.getter+0x28]^=1;
    Check(!f.Resolves(evidence),"return must establish RAX not another register");
    f.image=original;
    const int32_t into_data=static_cast<int32_t>(f.object-(f.getter+0x3f));
    std::memcpy(f.image.data()+f.getter+0x3b,&into_data,4);
    Check(!f.Resolves(evidence),"initializer call cannot target non-code data");
    f.image=original;f.image[0x1800+shift+2]=5;
    Check(!f.Resolves(evidence),"destructor thunk must receive the matched cache object");
  }
}
void Decoder() {
  struct Case {std::vector<unsigned char> bytes;unsigned width, destination;bool memory;};
  for(const auto& c:std::vector<Case>{
      {{0x32,0xc0},8,0,false},{{0x40,0x32,0xf6},8,6,false},{{0x32,0xe4},8,0,false},
      {{0x40,0x32,0xe4},8,4,false},{{0x48,0x32,0xc0},8,0,false},
      {{0x30,0xe4},8,0,false},{{0x40,0x30,0xf6},8,6,false},
      {{0x45,0x1b,0xf6},32,14,false},{{0x4d,0x1b,0xf6},64,14,false},
      {{0x19,0xd2},32,2,false},{{0x18,0xe4},8,0,false},{{0x1a,0xe4},8,0,false},
      {{0x32,0x04,0x24},8,0,true},{{0x30,0x04,0x24},8,16,true},
      {{0x1a,0x04,0x24},8,0,true},{{0x19,0x04,0x24},32,16,true}}) {
    std::vector<unsigned char> image(0x100,0xcc);std::copy(c.bytes.begin(),c.bytes.end(),image.begin()+0x10);
    const std::vector<mg::Range> ranges{{0x10,0x40,true}};
    mg::View view{image,ranges,{}};
    const auto d=mg::Decode(view,0x10,0x10+static_cast<uint32_t>(c.bytes.size()));
    Check(d.next==0x10+c.bytes.size()&&d.width==c.width&&d.memory==c.memory,"instruction length/width/addressing");
    for(unsigned r=0;r<16;++r)Check(mg::WritesRegister(d,r)==(r==c.destination),"correct write/partial-register alias");
    for(size_t n=0;n<c.bytes.size();++n)
      Check(mg::Length(view,0x10,0x10+n)==0,"truncation never becomes an instruction");
  }
}
}
int main() {
  Reader();Accessor();Decoder();
  std::cout<<"fg_preset_cached_test: "<<checks<<" checks passed (synthetic, no DLL execution)\n";
}
