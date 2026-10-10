// SPDX-License-Identifier: MIT
#pragma once

// Test-only PE64 relocation support. Input is a file-mapped private image, not
// a LoadLibrary handle; no vendor instructions are called. Validate the entire
// relocation plan before touching the copy and reject ambiguous target writes.
#include "../../../src/addons/mfgunlock/fatbin.hpp"
#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace mfgunlock::validation {
inline size_t RelocateMappedProvider(std::span<unsigned char> image) {
  const auto check = [](bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
  };
  const auto fits = [&](size_t at, size_t bytes) {
    return at <= image.size() && bytes <= image.size() - at;
  };
  check(fits(0, 64) && image[0]=='M' && image[1]=='Z', "relocation DOS header");
  const size_t nt=fatbin::ReadU32(image.data()+0x3c), opt=nt+24;
  check(fits(nt,24) && fits(opt,160) && fatbin::ReadU32(image.data()+nt)==0x4550 &&
            fatbin::ReadU16(image.data()+nt+4)==0x8664 &&
            fatbin::ReadU16(image.data()+nt+20)>=160 &&
            fatbin::ReadU16(image.data()+opt)==0x20b &&
            fatbin::ReadU32(image.data()+opt+56)==image.size() &&
            fatbin::ReadU32(image.data()+opt+108)>=6, "relocation PE64 header");
  const uint64_t preferred=fatbin::ReadU64(image.data()+opt+24);
  const uint64_t delta=reinterpret_cast<uintptr_t>(image.data())-preferred;
  const size_t begin=fatbin::ReadU32(image.data()+opt+152);
  const size_t count=fatbin::ReadU32(image.data()+opt+156);
  check(fits(begin,count) && (begin!=0 || count==0), "relocation directory bounds");
  std::vector<size_t> targets;
  for (size_t at=begin;at<begin+count;) {
    check(begin+count-at>=8, "relocation block header");
    const size_t page=fatbin::ReadU32(image.data()+at);
    const size_t block=fatbin::ReadU32(image.data()+at+4);
    check(block>=8 && block<=begin+count-at && block%2==0, "relocation block bounds");
    for (size_t i=8;i<block;i+=2) {
      const auto entry=fatbin::ReadU16(image.data()+at+i);
      if (!(entry>>12)) continue;
      check((entry>>12)==10, "relocation type must be DIR64");
      const size_t rva=page+(entry&0xfff);
      check(fits(rva,8) && rva>=fatbin::ReadU32(image.data()+opt+60) &&
                (rva+8<=begin || rva>=begin+count), "relocation target bounds");
      targets.push_back(rva);
      check(targets.size()<=1000000, "relocation target budget");
    }
    at+=block;
  }
  std::sort(targets.begin(),targets.end());
  for (size_t i=1;i<targets.size();++i)
    check(targets[i]>=targets[i-1]+8, "overlapping relocation targets");
  for (const auto rva:targets) {
    const uint64_t value=fatbin::ReadU64(image.data()+rva)+delta;
    std::memcpy(image.data()+rva,&value,sizeof(value));
  }
  return targets.size();
}
}  // namespace mfgunlock::validation
