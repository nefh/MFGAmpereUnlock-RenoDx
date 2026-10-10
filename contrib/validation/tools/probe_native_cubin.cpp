// SPDX-License-Identifier: MIT
// Production planners on input data, not Windows/NVIDIA code execution.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <fstream>
#include <iostream>
#include <array>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("mode --container or --provider required");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    if (std::string_view(argv[1]) == "--container") _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::vector<unsigned char> b;
    std::array<char, 65536> block{};
    while (std::cin) {
      std::cin.read(block.data(), block.size());
      const size_t n = static_cast<size_t>(std::cin.gcount());
      if (b.size() + n > 128u * 1024u * 1024u) throw std::runtime_error("input limit");
      b.insert(b.end(), block.data(), block.data() + n);
    }
    if (std::string_view(argv[1]) == "--container") {
      std::vector<unsigned char> out;
      std::string reason;
      if (!mfgunlock::cubin::RetargetOpaqueSm89To86(b, out, reason)) {
        std::cerr << reason << '\n'; return 2;
      }
      std::cout.write(reinterpret_cast<const char*>(out.data()), out.size());
      return 0;
    }
    if (std::string_view(argv[1]) != "--provider") throw std::runtime_error("unknown mode");
    namespace p = mfgunlock::provider;
    namespace f = mfgunlock::fatbin;
    const auto module = static_cast<HMODULE>(b.data());
    provider_mock::regions = {{module, b.size()}};
    provider_mock::paths[module] = "read-only-mapped-copy.dll";
    p::internal::Image image;
    if (!p::internal::InspectImage(module, image) || image.bytes != b.size())
      throw std::runtime_error("invalid mapped image");
    const auto nt = f::ReadU32(b.data() + 60);
    const auto dir = f::ReadU32(b.data() + nt + 24 + 112);
    const auto dirbytes = f::ReadU32(b.data() + nt + 24 + 116);
    const auto fits = [&](size_t at, size_t size) { return at <= b.size() && size <= b.size() - at; };
    if (!fits(dir, dirbytes) || dirbytes < 40) throw std::runtime_error("export bounds");
    const auto* e = b.data() + dir;
    const auto nfunc = f::ReadU32(e+20), nname=f::ReadU32(e+24);
    const auto funcs=f::ReadU32(e+28), names=f::ReadU32(e+32), ords=f::ReadU32(e+36);
    if (nname>8192 || nfunc>8192 || !fits(funcs,size_t{nfunc}*4) ||
        !fits(names,size_t{nname}*4) || !fits(ords,size_t{nname}*2))
      throw std::runtime_error("export table bounds");
    size_t bound=0;
    for (size_t i=0;i<nname;++i) {
      const auto name=f::ReadU32(b.data()+names+4*i);
      const auto ordinal=f::ReadU16(b.data()+ords+2*i);
      if (ordinal>=nfunc || !fits(name,1)) throw std::runtime_error("export ordinal/name");
      const auto end=static_cast<const unsigned char*>(std::memchr(b.data()+name,0,std::min<size_t>(256,b.size()-name)));
      if (!end) throw std::runtime_error("export name limit");
      const auto rva=f::ReadU32(b.data()+funcs+ordinal*4);
      if (!fits(rva,1) || (rva>=dir && rva-dir<dirbytes)) continue;
      provider_mock::exports[{module,std::string(reinterpret_cast<const char*>(b.data()+name),static_cast<size_t>(end-(b.data()+name)))}]=reinterpret_cast<FARPROC>(b.data()+rva);
      ++bound;
    }
    mfgunlock::g_enabled=true;
    mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);
    const auto before=b;
    const bool identified=p::IsDlssgProvider(module);
    const bool prepared=p::PrepareProvider(module);
    size_t changed=0;
    for (size_t i=0;i<b.size();++i) changed+=b[i]!=before[i];
    size_t bins=0,hidden=0;
    for (const auto& v:p::internal::g_providers) {bins+=v.fatbins;hidden+=v.hidden_cubins;}
    const auto writes=provider_mock::protection_calls;
    const bool repeated=prepared && p::PrepareProvider(module);
    const bool idem=!prepared || (repeated && writes==provider_mock::protection_calls);
    for (const auto& r:p::internal::g_rejected) std::cerr<<r.reason<<'\n';
    p::Restore();
    const bool restored=before==b && p::internal::g_providers.empty();
    std::cout<<"{\"identified\":"<<(identified?"true":"false")
      <<",\"prepared\":"<<(prepared?"true":"false")<<",\"exports_bound\":"<<bound
      <<",\"fatbins\":"<<bins<<",\"hidden_cubins\":"<<hidden<<",\"changed_bytes\":"<<changed
      <<",\"idempotent\":"<<(idem?"true":"false")<<",\"rollback_identical\":"<<(restored?"true":"false")
      <<",\"gpu_execution\":\"NOT_RUN\"}\n";
    return idem&&restored?0:3;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
