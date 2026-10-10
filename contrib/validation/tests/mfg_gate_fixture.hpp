// SPDX-License-Identifier: MIT
#pragma once
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <stdexcept>
namespace mfg_gate_fixture {
using Bytes=std::vector<unsigned char>;
struct Image {
  Bytes bytes=Bytes(0x8000,0);
  std::vector<std::pair<uint32_t,uint32_t>> functions;
  uint32_t cap_site=0,run_site=0,flag_load=0,cap_branch=0,ceiling_mov=0;
  HMODULE Module() {return reinterpret_cast<HMODULE>(bytes.data());}
  void U32(size_t at,uint32_t v){std::memcpy(bytes.data()+at,&v,4);}
  void Put(uint32_t& at,std::initializer_list<unsigned char> b){for(auto c:b)bytes[at++]=c;}
  void Imm(uint32_t& at,uint32_t v){U32(at,v);at+=4;}
  void Call(uint32_t& at,uint32_t to){Put(at,{0xe8});Imm(at,to-(at+4));}
  void Lea(uint32_t& at,unsigned reg,uint32_t to){Put(at,{static_cast<unsigned char>(reg>=8?0x4c:0x48),0x8d,static_cast<unsigned char>(0x05+((reg&7)<<3))});Imm(at,to-(at+4));}
  void Named(uint32_t at,const char* text){std::memcpy(bytes.data()+at,text,std::strlen(text)+1);}
  void Func(uint32_t start,uint32_t end){functions.emplace_back(start,end);}
  void Advertise(uint32_t start,unsigned ceiling){
    uint32_t p=start;Put(p,{0x4d,0x8b,0xf8,0x48,0x8b,0x01,0x48,0x8b,0x40,0x38,0xff,0x15});Imm(p,0x5800-(p+4));
    Put(p,{0x8b,0xc8});Call(p,0x4500);Put(p,{0x8b,0xe8});cap_site=p+2;Put(p,{0x81,0xfd});Imm(p,0x1b0);
    cap_branch=p;Put(p,{0x7c,0x07});ceiling_mov=p;Put(p,{0xbf});Imm(p,ceiling);Put(p,{0xeb,0x05,0xbf});Imm(p,1);
    Put(p,{0x49,0x8b,0x07,0x44,0x8b,0xc7});Lea(p,2,0x5100);Put(p,{0x49,0x8b,0xcf,0x48,0x8b,0x40,0x20,0xff,0x15});Imm(p,0x5800-(p+4));Put(p,{0xc3});Func(start,p);
  }
  explicit Image(unsigned ceiling=3,bool duplicate=false) {
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;std::memcpy(bytes.data(),&dos,sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.TimeDateStamp=0x12345678;nt.FileHeader.NumberOfSections=2;nt.FileHeader.SizeOfOptionalHeader=sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt.OptionalHeader.SizeOfImage=static_cast<DWORD>(bytes.size());nt.OptionalHeader.SizeOfHeaders=0x400;
    nt.OptionalHeader.NumberOfRvaAndSizes=16;std::memcpy(bytes.data()+0x80,&nt,sizeof(nt));
    IMAGE_SECTION_HEADER t{};t.VirtualAddress=0x1000;t.Misc.VirtualSize=0x4000;t.SizeOfRawData=t.Misc.VirtualSize;t.PointerToRawData=t.VirtualAddress;t.Characteristics=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;
    IMAGE_SECTION_HEADER d{};d.VirtualAddress=0x5000;d.Misc.VirtualSize=0x3000;d.SizeOfRawData=d.Misc.VirtualSize;d.PointerToRawData=d.VirtualAddress;d.Characteristics=IMAGE_SCN_MEM_READ;
    std::memcpy(bytes.data()+0x80+sizeof(nt),&t,sizeof(t));std::memcpy(bytes.data()+0x80+sizeof(nt)+sizeof(t),&d,sizeof(d));
    U32(0x80+24+192,0x5a00);U32(0x80+24+196,128);U32(0x5a00,128);U32(0x5a00+120,0x5800);
    Named(0x5100,"DLSSG.MultiFrameCountMax");Named(0x5200,"EndpointCore::Create");Named(0x5300,"EndpointCore::Evaluate");Named(0x5400,"EndpointCoreInputs::ComputeAndValidateTimeFactor");
    Advertise(0x1000,ceiling);if(duplicate)Advertise(0x1200,ceiling);
    uint32_t p=0x2000;Put(p,{0x48,0x8b,0xf9});Lea(p,9,0x5200);
    Put(p,{0x48,0x8b,0x4f,0x18,0x48,0x8b,0x01,0x48,0x8b,0x40,0x38,0xff,0x15});Imm(p,0x5800-(p+4));run_site=p+1;
    Put(p,{0x3d});Imm(p,0x1b0);Put(p,{0x0f,0x93,0xc0,0x88,0x47,0x28,0xc3});Func(0x2000,p);
    p=0x3000;Put(p,{0x48,0x8b,0xf9});Lea(p,9,0x5300);flag_load=p;Put(p,{0x0f,0xb6,0x57,0x28,0x48,0x8d,0x4c,0x24,0x50});Call(p,0x4000);Put(p,{0xc3});Func(0x3000,p);
    p=0x4000;Lea(p,9,0x5400);Put(p,{0x44,0x8b,0x41,0x40,0x84,0xd2,0x74,0x0b,0xbe});Imm(p,ceiling);
    Put(p,{0x44,0x3b,0xc6,0x76,0x08,0xc3,0x41,0x83,0xf8,0x01,0x74,0x01,0xc3,0xc3});Func(0x4000,p);
    // Sparse enum mapper. Tables are data even though in the executable section.
    p=0x4500;Put(p,{0x8d,0x81});Imm(p,static_cast<uint32_t>(-0x140));Put(p,{0x83,0xf8,0x70,0x0f,0x87});Imm(p,0x45b0-(p+4));
    Lea(p,8,0);Put(p,{0x48,0x98,0x41,0x0f,0xb6,0x84,0x00});Imm(p,0x46a0);Put(p,{0x41,0x8b,0x94,0x80});Imm(p,0x4650);Put(p,{0x49,0x03,0xd0,0xff,0xe2});
    const uint32_t enums[]={0x140,0x160,0x170,0x180,0x190,0x1a0,0x1b0};
    for(unsigned n=0;n<7;++n){p=0x4560+n*8;U32(0x4650+4*n,p);Put(p,{0xb8});Imm(p,enums[n]);Put(p,{0xc3});}
    p=0x45b0;Put(p,{0x33,0xd2,0xb8});Imm(p,0x07ffffff);Put(p,{0x81,0xf9});Imm(p,0x140);Put(p,{0x0f,0x4c,0xc2,0xc3});U32(0x4650+28,0x45b0);
    std::fill(bytes.begin()+0x46a0,bytes.begin()+0x46a0+0x71,7);
    for(unsigned n=0;n<7;++n)bytes[0x46a0+enums[n]-0x140]=static_cast<unsigned char>(n);
    Func(0x4500,0x4720);
    std::sort(functions.begin(),functions.end());
    for(size_t n=0;n<functions.size();++n){U32(0x6000+n*12,functions[n].first);U32(0x6004+n*12,functions[n].second);U32(0x6008+n*12,0x5900);}
    U32(0x80+24+136,0x6000);U32(0x80+24+140,static_cast<uint32_t>(functions.size()*12));
    Bind();
  }
  void Bind(){provider_mock::regions.clear();provider_mock::regions.push_back({bytes.data(),bytes.size(),MEM_IMAGE,PAGE_EXECUTE_READ});}
};
inline void Reset(){using namespace mfgunlock;provider_mock::regions.clear();provider_mock::protection_calls=0;provider_mock::fail_protection_call=0;provider_mock::fail_protection_calls.clear();provider_mock::retains=0;provider_mock::flush_calls=0;provider_mock::fail_flush_call=0;provider_mock::corrupt_flush_call=0;provider_mock::retain_ok=true;provider::g_create_seen=false;provider::internal::g_registry_failed=false;provider::internal::g_comparison_restore_failed=false;g_enabled=true;architecture::Configure(Architecture::kAmpere);}
}
