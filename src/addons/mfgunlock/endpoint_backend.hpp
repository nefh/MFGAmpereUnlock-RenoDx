// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <string>
#include <vector>
#include <cstring>
#include <initializer_list>
#include <cctype>

#include "./ptx_retarget.hpp"
#include "./provider_profile.hpp"

namespace mfgunlock::endpoint {

using Region = profiles::Region;
// Patch bytes are part of a validated plan, not assumed to be NOPs. Modern
// dual factories remove JLE; legacy flat factories redirect JE to its PTX arm.
struct Selector {
  uint32_t rva=0;
  std::array<unsigned char,2> before{};
  std::array<unsigned char,2> after{0x90,0x90};
};
using VtableBinding = profiles::VtableBinding;
inline constexpr auto kTimestamp = profiles::kTimestamp;
inline constexpr auto kImageBytes = profiles::kImageBytes;
inline constexpr auto& kCode = profiles::kEndpointCode;
inline constexpr auto& kSelectors = profiles::kEndpointSelectors;
inline constexpr auto& kPayloads = profiles::kEndpointPayloads;

inline bool Matches(std::span<const unsigned char> bytes, const Region& region) {
  return bytes.size() == region.bytes &&
         ptx::SourceFingerprint({reinterpret_cast<const char*>(bytes.data()), bytes.size()}) ==
             region.hash;
}

inline bool QualifiedCode(std::span<const unsigned char> image, uint32_t timestamp) {
  if (timestamp != kTimestamp || image.size() != kImageBytes) return false;
  for (const auto& region : kCode) {
    if (region.rva > image.size() || region.bytes > image.size() - region.rva ||
        !Matches(image.subspan(region.rva, region.bytes), region)) return false;
  }
  for (const auto& site : kSelectors) {
    if (site.rva > image.size() || site.before.size() > image.size() - site.rva ||
        !std::equal(site.before.begin(), site.before.end(), image.begin() + site.rva)) return false;
  }
  // Constructors publish these vtables; bind their variant-specific loaders.
  const auto base = reinterpret_cast<uintptr_t>(image.data());
  for (const auto& binding : profiles::kEndpointVtables) {
    if (binding.rva > image.size() || sizeof(uint64_t) > image.size() - binding.rva ||
        fatbin::ReadU64(image.data() + binding.rva) != base + binding.target_rva) return false;
  }
  return true;
}

inline bool PreparedPayload(std::span<const unsigned char> before,
                            std::span<const unsigned char> after, const Region& region) {
  if (!Matches(before, region) || after.size() != before.size()) return false;
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  return fatbin::Parse(after, entries, end) && entries.size() == 1 &&
         entries.front().kind == 1 && entries.front().architecture == 75;
}


// Location-independent selection of the existing PTX network implementations.
// The factory's three-way architecture dispatch, constructor RTTI/vtable and
// each loader's fatbin/name/length callsites must all agree. File version,
// timestamp, original RVAs and whole-function hashes do not select this path.
struct PayloadReference {
  uint32_t rva = 0;
  uint32_t bytes = 0;
  std::string entry_name;
};
enum class Family { kUnknown, kModernDual, kLegacyFlat };
inline const char* FamilyName(Family f) {
  switch(f) {
    case Family::kModernDual:return "MODERN_DUAL_FACTORY";
    case Family::kLegacyFlat:return "LEGACY_FLAT_FACTORY";
    default:return "UNRESOLVED";
  }
}
struct HostWitness { uint32_t rva=0; std::vector<unsigned char> bytes; };
struct StructuralPlan {
  Family family=Family::kUnknown;
  std::vector<HostWitness> witnesses;
  std::array<Selector, 2> selectors{};
  std::array<VtableBinding, 2> loaders{};
  std::vector<PayloadReference> payloads;
  std::array<size_t, 2> network_counts{};
};
struct ImageRange { uint32_t rva = 0; uint32_t bytes = 0; bool executable = false; };

namespace structural {

struct ImageView {
  std::span<const unsigned char> image;
  std::span<const ImageRange> ranges;
  std::vector<std::pair<uint32_t, uint32_t>> functions;

  bool Fits(size_t at, size_t n) const { return at <= image.size() && n <= image.size() - at; }
  bool InSection(size_t at, size_t n, bool code) const {
    if (!Fits(at, n) || n == 0) return false;
    for (const auto& r : ranges) {
      if (r.executable == code && at >= r.rva && at - r.rva <= r.bytes &&
          n <= r.bytes - (at - r.rva)) return true;
    }
    return false;
  }
  uint32_t U32(size_t at) const { return Fits(at, 4) ? fatbin::ReadU32(image.data() + at) : 0; }
  uint64_t U64(size_t at) const { return Fits(at, 8) ? fatbin::ReadU64(image.data() + at) : 0; }
  bool Bytes(size_t at, std::initializer_list<unsigned char> bytes) const {
    return Fits(at, bytes.size()) && std::equal(bytes.begin(), bytes.end(), image.begin() + at);
  }
  uint32_t Relative(size_t at, size_t disp, size_t size) const {
    if (!Fits(at + disp, 4)) return 0;
    int32_t delta = 0; std::memcpy(&delta, image.data() + at + disp, 4);
    const int64_t target = static_cast<int64_t>(at + size) + delta;
    return target > 0 && static_cast<uint64_t>(target) < image.size()
        ? static_cast<uint32_t>(target) : 0;
  }
  uint32_t PointerRva(uint64_t pointer) const {
    const auto base = reinterpret_cast<uintptr_t>(image.data());
    return pointer > base && pointer - base < image.size()
        ? static_cast<uint32_t>(pointer - base) : 0;
  }
  std::string_view String(uint32_t at, size_t limit = 256) const {
    if (!InSection(at, 1, false)) return {};
    size_t count = 0;
    for (const auto& r : ranges) {
      if (!r.executable && at >= r.rva && at - r.rva < r.bytes)
        count = std::max(count, std::min(limit, size_t{r.bytes - (at - r.rva)}));
    }
    if (!count) return {};
    const auto* zero = static_cast<const unsigned char*>(std::memchr(image.data()+at, 0, count));
    if (!zero || !InSection(at, static_cast<size_t>(zero - image.data() - at) + 1, false)) return {};
    return {reinterpret_cast<const char*>(image.data()+at), static_cast<size_t>(zero-image.data()-at)};
  }
  std::pair<uint32_t, uint32_t> Function(uint32_t start) const {
    for (const auto& r : functions) if (r.first == start) return r;
    return {};
  }
  bool ReadFunctions() {
    if (!Fits(0x3c, 4)) return false;
    const size_t nt = U32(0x3c), opt = nt + 24;
    if (!Fits(opt, 144) || U32(nt) != 0x4550 || U32(opt + 108) < 4) return false;
    size_t at = U32(opt + 112 + 3*8), n = U32(opt + 116 + 3*8);
    if (!n || n % 12 || n / 12 > 65536 || !InSection(at, n, false)) return false;
    uint32_t previous_end = 0;
    for (size_t i = 0; i < n; i += 12) {
      const auto begin = U32(at+i), end = U32(at+i+4);
      if (begin >= end || begin < previous_end || !InSection(begin, end-begin, true)) return false;
      functions.emplace_back(begin,end); previous_end = end;
    }
    return true;
  }
};

// Restricted x64 length decoder for the scalar MSVC factory/loader contract.
// Start only at a .pdata function boundary. Unknown opcodes fail the optional
// structural match; they are never skipped or interpreted as branch operands.
inline size_t InstructionBytes(const ImageView& v, size_t at, size_t end) {
  if (at >= end || end > v.image.size()) return 0;
  const size_t begin=at; bool wide=false, operand16=false;
  for (unsigned n=0;n<4 && at<end;++n) {
    const auto b=v.image[at];
    if (b>=0x40 && b<=0x4f) { wide=(b&8)!=0; ++at; }
    else if (b==0x66) { operand16=true; ++at; }
    else if (b==0xf2 || b==0xf3) ++at;
    else break;
  }
  if (at>=end) return 0;
  const auto op=v.image[at++]; size_t immediate=0; bool modrm=false;
  if ((op>=0x50 && op<=0x5f) || op==0x90 || op==0x98 || op==0x99 || op==0xc3 || op==0xcc) {}
  else if (op>=0xb8 && op<=0xbf) immediate=wide?8:operand16?2:4;
  else if ((op>=0x70 && op<=0x7f) || op==0xeb || op==0x6a) immediate=1;
  else if (op==0xe8 || op==0xe9 || op==0x68) immediate=4;
  else if (op==0xc2) immediate=2;
  else if (op==0x0f) {
    if (at>=end) return 0;
    const auto second=v.image[at++];
    if (second>=0x80 && second<=0x8f) immediate=4;
    else if ((second>=0x40 && second<=0x4f) || (second>=0x90 && second<=0x9f) ||
        second==0x10 || second==0x11 || second==0x28 || second==0x29 || second==0x57 ||
        second==0xb6 || second==0xb7 || second==0xbe || second==0xbf || second==0xaf || second==0x1f)
      modrm=true;
    else return 0;
  } else if (op==0x25 || op==0x3d || op==0x05 || op==0x0d || op==0x2d || op==0xa9)
    immediate=operand16?2:4;
  else if (op==0x01 || op==0x03 || op==0x09 || op==0x0b || op==0x21 || op==0x23 ||
      op==0x29 || op==0x2b || op==0x31 || op==0x33 || op==0x38 || op==0x39 || op==0x3a || op==0x3b ||
      op==0x63 || op==0x84 || op==0x85 || (op>=0x88 && op<=0x8d) || op==0xff ||
      op==0xc6 || op==0xc7 || op==0x80 || op==0x81 || op==0x83 || op==0xc0 || op==0xc1 ||
      (op>=0xd0 && op<=0xd3) || op==0xf6 || op==0xf7) {
    modrm=true;
    if (op==0xc6 || op==0x80 || op==0x83 || op==0xc0 || op==0xc1) immediate=1;
    if (op==0xc7 || op==0x81) immediate=operand16?2:4;
  } else return 0;
  if (modrm) {
    if (at>=end) return 0;
    const auto m=v.image[at++]; const auto mod=m>>6, rm=m&7;
    if ((op==0xf6 || op==0xf7) && ((m>>3)&7)==0) immediate=op==0xf6?1:operand16?2:4;
    if (mod!=3 && rm==4) {
      if (at>=end) return 0;
      const auto sib=v.image[at++];
      if (mod==0 && (sib&7)==5) at+=4;
    } else if (mod==0 && rm==5) at+=4;
    if (mod==1) ++at;
    if (mod==2) at+=4;
  }
  at+=immediate;
  return at<=end && at-begin<=15 ? at-begin:0;
}

inline bool Instructions(const ImageView& v, uint32_t begin, uint32_t end,
                         std::vector<uint32_t>& starts) {
  starts.clear();
  if (!v.InSection(begin,end-begin,true)) return false;
  for (size_t at=begin;at<end;) {
    const auto n=InstructionBytes(v,at,end);
    if (!n) return false;
    starts.push_back(static_cast<uint32_t>(at)); at+=n;
  }
  return true;
}

struct Arm {
  uint32_t allocator = 0, constructor = 0, end = 0, join = 0;
  unsigned result_register = 0;
};

// Parse complete small allocation/constructor arms. Relative displacements and
// stack spills are operands, not wildcards authorizing an arbitrary branch.
inline bool ReadArm(const ImageView& v, uint32_t begin, uint32_t end, Arm& out,
                    bool exit_jump = true) {
  out = {};
  size_t at = begin;
  if (!v.Bytes(at, {0xe8})) return false;
  out.allocator = v.Relative(at, 1, 5); at += 5;
  if (!v.Function(out.allocator).first) return false;
  if (v.Bytes(at, {0x48,0x89,0x44,0x24}) && v.Fits(at,5)) at += 5;
  else if (v.Bytes(at,{0x48,0x89,0x84,0x24}) && v.Fits(at,8)) at += 8;
  else return false;

  // MSVC may clear a shared nonvolatile zero register before the allocation
  // test, rather than clearing each arm's result on the null branch. None of
  // these allocation/null-handling instructions are modified by the selector.
  const auto clear_register = [&](size_t pos, unsigned& reg) -> size_t {
    size_t n = 0; unsigned extension = 0;
    if (v.Bytes(pos,{0x45})) { n = 1; extension = 8; }
    if (!v.Fits(pos+n,2) || v.image[pos+n]!=0x33) return 0;
    const auto m=v.image[pos+n+1];
    if ((m&0xc0)!=0xc0 || ((m>>3)&7)!=(m&7)) return 0;
    reg=(m&7)+extension;
    if (reg!=3 && reg!=5 && reg!=6 && reg!=7 && reg<12) return 0;
    return n+2;
  };
  unsigned zero = 0;
  at += clear_register(at,zero);
  if (!v.Bytes(at,{0x48,0x85,0xc0,0x74}) || !v.Fits(at,5)) return false;
  const int64_t null_path = static_cast<int64_t>(at+5) + static_cast<int8_t>(v.image[at+4]);
  at += 5;
  if (!v.Bytes(at,{0x48,0x8b,0xc8,0xe8})) return false;
  out.constructor=v.Relative(at+3,1,5); at+=8;
  if (!v.Function(out.constructor).first || !v.Fits(at,3)) return false;
  // mov <nonvolatile result>,rax. Decode the register operand, not a fixed
  // compiler choice of RSI/RDI/R14/R15.
  const auto rex=v.image[at], modrm=v.image[at+2];
  if ((rex!=0x48 && rex!=0x4c) || v.image[at+1]!=0x8b || (modrm&0xc7)!=0xc0)
    return false;
  const unsigned reg=((modrm>>3)&7) + ((rex&4)?8:0);
  if (reg!=3 && reg!=5 && reg!=6 && reg!=7 && reg<12) return false;
  out.result_register=reg; at+=3;

  if (null_path==static_cast<int64_t>(at)) {
    // Compiler hoisted initialization of the result. The existing null edge
    // joins directly after the constructor result; preserve it unchanged.
  } else {
    if (null_path!=static_cast<int64_t>(at+2) || !v.Bytes(at,{0xeb}) || !v.Fits(at,2)) return false;
    const int64_t merge=static_cast<int64_t>(at+2)+static_cast<int8_t>(v.image[at+1]);
    at+=2;
    unsigned cleared=0;
    const auto n=clear_register(at,cleared);
    if (n) {
      if (cleared!=reg) return false;
      at+=n;
    } else {
      // mov result,<nonvolatile zero register>, observed in shared-zero arms.
      if (!v.Fits(at,3)) return false;
      const auto prefix=v.image[at], m=v.image[at+2];
      if (prefix<0x48 || prefix>0x4f || v.image[at+1]!=0x8b || (m&0xc0)!=0xc0)
        return false;
      const unsigned dst=((m>>3)&7)+((prefix&4)?8:0), src=(m&7)+((prefix&1)?8:0);
      if (dst!=reg || (src!=3 && src!=5 && src!=6 && src!=7 && src<12)) return false;
      at+=3;
    }
    if (merge!=static_cast<int64_t>(at)) return false;
  }
  if (exit_jump) {
    if (!v.Bytes(at,{0xeb}) || !v.Fits(at,2)) return false;
    const int64_t join=static_cast<int64_t>(at+2)+static_cast<int8_t>(v.image[at+1]);
    if (join<=static_cast<int64_t>(at+2) || join>end) return false;
    out.join=static_cast<uint32_t>(join); at+=2;
  } else out.join=static_cast<uint32_t>(at);
  out.end=static_cast<uint32_t>(at);
  return at<=end && v.InSection(begin,at-begin,true);
}

inline bool ConstructorVtable(const ImageView& v, uint32_t constructor,
                              std::string_view type_name, uint32_t& table) {
  const auto f = v.Function(constructor);
  if (!f.first || f.second - f.first > 32768) return false;
  size_t matches = 0;
  // The inspected C++ constructors store their final vptr in [rbx]. Find and
  // validate the actual RIP operand, including MSVC relative RTTI self pointer.
  std::vector<uint32_t> starts;
  if (!Instructions(v, f.first, f.second, starts)) return false;
  for (const auto at : starts) {
    if (at + 10 > f.second) continue;
    if (!v.Bytes(at,{0x48,0x8d,0x05}) || !v.Bytes(at+7,{0x48,0x89,0x03})) continue;
    const auto candidate = v.Relative(at,3,7);
    if (candidate < 8 || !v.InSection(candidate-8,56,false)) continue;
    const auto locator = v.PointerRva(v.U64(candidate-8));
    if (!locator || !v.InSection(locator,24,false) || v.U32(locator)!=1 ||
        v.U32(locator+4)!=0 || v.U32(locator+8)!=0 || v.U32(locator+20)!=locator) continue;
    const auto type = v.U32(locator+12);
    if (!v.InSection(type,17,false) || v.String(type+16)!=type_name) continue;
    ++matches; table = candidate;
  }
  return matches == 1;
}

inline bool PtxHasEntry(std::span<const unsigned char> container, unsigned target,
                        std::string_view name) {
  std::vector<fatbin::Entry> entries; size_t end=0;
  if (!fatbin::Parse(container,entries,end) || name.empty() || name.size()>256) return false;
  for (const unsigned char c : name)
    if (!(std::isalnum(c) || c=='_' || c=='$')) return false;
  const fatbin::Entry* selected=nullptr;
  for (const auto& e:entries) {
    if (e.kind!=1 || e.architecture!=target) continue;
    if (selected) return false;
    selected=&e;
  }
  if (!selected) return false;
  std::vector<unsigned char> raw;
  if (selected->compressed_bytes) {
    raw.resize(selected->unpacked_bytes);
    if (!fatbin::Lz4BlockDecompress(container.data()+selected->PayloadOffset(),
          selected->compressed_bytes,raw.data(),raw.size())) return false;
  } else raw.assign(container.begin()+selected->PayloadOffset(),container.begin()+selected->End());
  const auto nul=std::find(raw.begin(),raw.end(),0);
  if (nul==raw.end()) return false;
  std::string text(reinterpret_cast<const char*>(raw.data()),static_cast<size_t>(nul-raw.begin()));
  bool comment=false, quoted=false;
  for (size_t i=0;i<text.size();++i) {
    const auto c=text[i];
    if (comment) {
      text[i]=' ';
      if (c=='*' && i+1<text.size() && text[i+1]=='/') { text[++i]=' ';comment=false; }
    } else if (quoted) {
      text[i]=' ';
      if (c=='\\' && i+1<text.size()) text[++i]=' ';
      else if (c=='"') quoted=false;
    } else if (c=='"') { quoted=true;text[i]=' '; }
    else if (c=='/' && i+1<text.size() && text[i+1]=='*') {
      text[i]=' ';text[++i]=' ';comment=true;
    } else if (c=='/' && i+1<text.size() && text[i+1]=='/') {
      while(i<text.size() && text[i]!='\n') text[i++]=' ';
      if (i==text.size()) break;
    }
  }
  if (comment || quoted) return false;
  // Entry declaration, not a match against a filename or arbitrary label.
  const std::string pattern = ".entry " + std::string(name);
  size_t matches=0;
  for (size_t at=0;(at=text.find(pattern,at))!=text.npos;at+=pattern.size()) {
    const auto pos=at+pattern.size();
    if (pos<text.size() && (text[pos]=='(' || text[pos]==' ' || text[pos]=='\n')) ++matches;
  }
  return matches==1;
}

inline bool LoaderPayloads(const ImageView& v, uint32_t loader,
                           std::vector<PayloadReference>& refs) {
  const auto f = v.Function(loader);
  if (!f.first || f.second-f.first>32768) return false;
  std::vector<uint32_t> instructions;
  if (!Instructions(v,f.first,f.second,instructions)) return false;
  uint32_t load_call=0, payload=0, length=0;
  std::string_view name;
  const auto initial=refs.size();
  for (const auto at:instructions) {
    const auto n=InstructionBytes(v,at,f.second);
    if (v.Bytes(at,{0x4c,0x8d,0x0d})) {
      name=v.String(v.Relative(at,3,7));
    } else if (v.Bytes(at,{0x41,0xb8})) {
      length=v.U32(at+2);
    } else if (v.Bytes(at,{0x48,0x8d,0x15})) {
      const auto dest=v.Relative(at,3,7);
      if (v.InSection(dest,16,false) && v.U32(dest)==fatbin::kMagic) {
        if (payload) return false;
        payload=dest;
      } else if (payload) return false;
    } else if (v.Bytes(at,{0xe8})) {
      const auto called=v.Relative(at,1,5);
      if (payload) {
        if (!length || length>fatbin::kMaxFatbinBytes || !v.InSection(payload,length,false) ||
            v.U64(payload+8)!=length-16 || name.empty() || !v.Function(called).first ||
            (load_call && called!=load_call) || !PtxHasEntry(v.image.subspan(payload,length),89,name))
          return false;
        if (refs.size()>=512 || std::any_of(refs.begin()+initial,refs.end(),
             [&](const auto& r){return r.rva==payload && r.entry_name==name;})) return false;
        refs.push_back({payload,length,std::string(name)}); load_call=called;
      } else if (load_call && called==load_call) return false;
      payload=length=0; name={};
    } else if (payload) {
      // Once the tuple points at a fatbin, accept only argument-independent
      // field loads/stack stores and RCX=this until the direct loading call.
      // Any clobber/branch/indirect call makes dataflow unproven.
      if (!v.Bytes(at,{0x8b,0x83}) && !v.Bytes(at,{0x89,0x44,0x24}) &&
          !v.Bytes(at,{0x48,0x8b,0xcb})) return false;
    } else {
      // Unknown instructions before a fatbin can clobber arguments. Keep a
      // pending R8/R9 value only across the observed field loads, stack stores
      // and this-pointer assignment, never across an unmodelled definition.
      if (!v.Bytes(at,{0x8b,0x83}) && !v.Bytes(at,{0x89,0x44,0x24}) &&
          !v.Bytes(at,{0x48,0x8b,0xcb})) { length=0; name={}; }
      // Calls and control transfers end an argument-producing straight block.
      const auto op=v.image[at];
      if (op==0xe9 || op==0xeb || op==0xc3 || (op>=0x70 && op<=0x7f) ||
          (op==0x0f && n>=2 && v.image[at+1]>=0x80 && v.image[at+1]<=0x8f) ||
          v.Bytes(at,{0xff,0x15})) { length=0; name={}; }
    }
  }
  return !payload && load_call && refs.size()>initial;
}
}  // namespace structural

inline bool FindStructuralPlan(std::span<const unsigned char> image,
                               std::span<const ImageRange> ranges,
                               StructuralPlan& output, std::string& why) {
  output={};
  structural::ImageView v{image,ranges,{}};
  const auto reject=[&](const char* reason){why=reason;return false;};
  if (!v.ReadFunctions()) return reject("endpoint: invalid or missing function directory");
  StructuralPlan result;
  std::array<size_t,2> matches{};
  std::array<uint32_t,2> factories{};
  std::array<unsigned char,2> query_slots{};
  for (const auto& fn:v.functions) {
    if (fn.second-fn.first>32768) continue;
    std::vector<uint32_t> starts;
    if (!structural::Instructions(v,fn.first,fn.second,starts)) continue;
    for (const auto at:starts) {
      if (at+13>=fn.second) continue;
      if (!v.Bytes(at,{0x83,0xf8,0x59,0x7e}) || at<fn.first+18 ||
          !v.Bytes(at-18,{0x48,0x8b,0x01,0x48,0x8b,0x40}) ||
          image[at-12]==0 || image[at-12]>0x78 || (image[at-12]&7)!=0 ||
          !v.Bytes(at-11,{0xff,0x15}) ||
          !v.InSection(v.Relative(at-11,2,6),8,false) ||
          !v.Bytes(at-5,{0xb9}) ||
          !std::binary_search(starts.begin(), starts.end(), at-18) ||
          !std::binary_search(starts.begin(), starts.end(), at-5)) continue;
      const uint32_t ptx_begin=static_cast<uint32_t>(at+5);
      const int native_branch=static_cast<int>(at+5)+static_cast<int8_t>(image[at+4]);
      if (native_branch<=static_cast<int>(ptx_begin) || native_branch+2>=static_cast<int>(fn.second) ||
          !v.Bytes(native_branch,{0x7c})) continue;
      structural::Arm ptx_arm{},native_arm{};
      if (!structural::ReadArm(v,ptx_begin,fn.second,ptx_arm) ||
          ptx_arm.end!=static_cast<uint32_t>(native_branch) ||
          !structural::ReadArm(v,static_cast<uint32_t>(native_branch+2),fn.second,native_arm) ||
          ptx_arm.allocator!=native_arm.allocator || ptx_arm.join!=native_arm.join ||
          ptx_arm.result_register!=native_arm.result_register) continue;
      const int lower=native_branch+2+static_cast<int8_t>(image[native_branch+1]);
      structural::Arm lower_arm{};
      if (lower!=static_cast<int>(native_arm.end) || lower>=static_cast<int>(ptx_arm.join) ||
          !structural::ReadArm(v,static_cast<uint32_t>(lower),fn.second,lower_arm,false) ||
          lower_arm.allocator!=ptx_arm.allocator || lower_arm.join!=ptx_arm.join ||
          lower_arm.result_register!=ptx_arm.result_register) continue;
      size_t role=2; uint32_t table=0,native_table=0,lower_table=0;
      for (size_t r=0;r<2;++r) {
        const std::string base = ".?AVFlatten_EndpointDL" + std::to_string(r+1) + "Net";
        if (structural::ConstructorVtable(v,ptx_arm.constructor,base+"_89_PTX@@",table) &&
            structural::ConstructorVtable(v,native_arm.constructor,base+"_89@@",native_table) &&
            structural::ConstructorVtable(v,lower_arm.constructor,base+"@@",lower_table)) {
          if (role!=2) return reject("endpoint: ambiguous constructor role");
          role=r;
        }
      }
      if (role==2) continue;
      if (++matches[role]!=1) return reject("endpoint: duplicate factory selector");
      const auto loader=v.PointerRva(v.U64(table+32));
      const auto native_loader=v.PointerRva(v.U64(native_table+32));
      if (!loader || loader==native_loader || !v.Function(loader).first || !v.Function(native_loader).first)
        return reject("endpoint: invalid vtable loader binding");
      const auto count=result.payloads.size();
      if (!structural::LoaderPayloads(v,loader,result.payloads))
        return reject("endpoint: incomplete PTX loader payload coverage");
      result.network_counts[role]=result.payloads.size()-count;
      result.selectors[role]={static_cast<uint32_t>(at+3),{0x7e,image[at+4]}};
      result.loaders[role]={table+32,loader};
      factories[role]=fn.first;
      query_slots[role]=image[at-12];
    }
  }
  if (matches[0]!=1 || matches[1]!=1 || factories[0]!=factories[1] || query_slots[0]!=query_slots[1])
    return reject("endpoint: complete DL1/DL2 factory pair not found");
  result.family=Family::kModernDual;
  output=std::move(result); why.clear(); return true;
}

}  // namespace mfgunlock::endpoint
