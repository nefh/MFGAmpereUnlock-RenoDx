// SPDX-License-Identifier: MIT
#pragma once

#include "./endpoint_backend.hpp"

namespace mfgunlock::storage {

struct LoadSite {
  uint32_t lea_rva = 0;
  uint32_t length_rva = 0;
  uint32_t call_rva = 0;
  std::string entry_name;
};

// Prove direct, straight-line NGX payload loading tuples (RDX=data, R8D=size,
// R9=entry). A larger copy can then be addressed through an indirection cell
// in the old container: LEA RDX,[RIP+cell] -> MOV RDX,[RIP+cell]. Both are seven
// bytes and preserve the original displacement and every other argument.
// Extra/interior references veto relocation; raw scanning is a veto ONLY.
inline bool FindLoads(const endpoint::structural::ImageView& v, uint32_t payload,
                      uint32_t bytes, std::vector<LoadSite>& output, std::string& reason,
                      uint32_t source_sm = 89) {
  output.clear();
  const auto reject=[&](const char* why){output.clear();reason=why;return false;};
  if(bytes<16 || !v.InSection(payload,bytes,false) || v.U32(payload)!=fatbin::kMagic)
    return reject("SM75 storage: invalid original container");
  const auto in_payload=[&](uint64_t address){return address>=payload && address-payload<bytes;};
  std::vector<LoadSite> matches;
  for(const auto& fn:v.functions) {
    if(fn.second-fn.first>65536) continue;
    bool mentions=false;
    for(size_t at=fn.first;at+7<=fn.second;++at) {
      if(v.Bytes(at,{0x48,0x8d,0x15}) && in_payload(v.Relative(at,3,7))){mentions=true;break;}
    }
    if(!mentions)continue;
    std::vector<uint32_t> starts;
    if(!endpoint::structural::Instructions(v,fn.first,fn.second,starts))
      return reject("SM75 storage: undecodable loading function");
    uint32_t pointer=0,length=0,length_site=0,lea_site=0;
    std::string_view name;
    const auto reset=[&](){pointer=length=length_site=lea_site=0;name={};};
    for(const auto at:starts) {
      if(v.Bytes(at,{0x4c,0x8d,0x0d})) name=v.String(v.Relative(at,3,7));
      else if(v.Bytes(at,{0x41,0xb8})) {length=v.U32(at+2);length_site=at+2;}
      else if(v.Bytes(at,{0x48,0x8d,0x15})) {
        if(pointer) return reject("SM75 storage: pointer argument redefined");
        pointer=v.Relative(at,3,7);lea_site=at;
      } else if(v.Bytes(at,{0xe8})) {
        if(in_payload(pointer)) {
          if(pointer!=payload || length!=bytes || !length_site || name.empty() ||
              !v.Function(v.Relative(at,1,5)).first ||
              !endpoint::structural::PtxHasEntry(v.image.subspan(payload,bytes),source_sm,name))
            return reject("SM75 storage: incomplete loader pointer/length/name tuple");
          if(matches.size()>=64) return reject("SM75 storage: loadsite budget");
          matches.push_back({lea_site,length_site,at,std::string(name)});
        }
        reset();
      } else {
        // Field/stack stores do not write the pointer/length/name arguments.
        const bool independent=v.Bytes(at,{0x48,0x8b,0xcb}) || v.Bytes(at,{0x8b,0x83}) ||
            v.Bytes(at,{0x89,0x44,0x24}) || v.Bytes(at,{0xc7,0x44,0x24}) ||
            v.Bytes(at,{0x48,0x89,0x83}) || v.Bytes(at,{0x48,0x89,0x43});
        if(!independent) {
          if(in_payload(pointer)) return reject("SM75 storage: unmodelled instruction before load");
          reset();
        }
      }
    }
    if(in_payload(pointer))return reject("SM75 storage: unterminated load tuple");
  }
  if(matches.empty()) return reject("SM75 storage: no proven direct loadsites");

  // Check every materialized RIP-relative reference in readable code. A byte
  // match can only refuse a plan; only decoded instructions above authorize it.
  for(const auto& range:v.ranges) {
    if(!range.executable)continue;
    for(size_t at=range.rva;at+7<=size_t{range.rva}+range.bytes;++at) {
      const auto rex=v.image[at],op=v.image[at+1],m=v.image[at+2];
      if(rex<0x40 || rex>0x4f || (op!=0x8d && op!=0x8b && op!=0x89) || (m&0xc7)!=5)continue;
      if(!in_payload(v.Relative(at,3,7)))continue;
      if(!v.Bytes(at,{0x48,0x8d,0x15}) || v.Relative(at,3,7)!=payload ||
          std::count_if(matches.begin(),matches.end(),[&](const auto& site){return site.lea_rva==at;})!=1)
        return reject("SM75 storage: additional or interior code reference");
    }
  }
  const uint64_t base=reinterpret_cast<uintptr_t>(v.image.data());
  for(const auto& range:v.ranges) {
    if(range.executable)continue;
    for(size_t at=(size_t{range.rva}+7)&~size_t{7};at+8<=size_t{range.rva}+range.bytes;at+=8) {
      if(at>=payload && at-payload<bytes)continue;
      const auto value=v.U64(at);
      if(value>=base && in_payload(value-base))return reject("SM75 storage: indirect data reference");
    }
  }
  output=std::move(matches);reason.clear();return true;
}
}  // namespace mfgunlock::storage
