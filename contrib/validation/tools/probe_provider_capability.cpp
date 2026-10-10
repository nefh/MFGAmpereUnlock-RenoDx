// SPDX-License-Identifier: MIT
// Read-only production-planner probe. The input is a privately mapped copy of a
// provider PE. NVIDIA/vendor code is never executed by this executable.
#include "../../../src/addons/mfgunlock/provider.hpp"

#include "mapped_provider_image.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
namespace p = mfgunlock::provider;
namespace f = mfgunlock::fatbin;
namespace a = mfgunlock::architecture;

mfgunlock::Architecture ParseArchitecture(std::string_view value) {
  if (value == "Ampere") return mfgunlock::Architecture::kAmpere;
  if (value == "Turing") return mfgunlock::Architecture::kTuring;
  throw std::runtime_error("architecture must be Ampere or Turing");
}

void PrintJsonString(const std::string& value) {
  std::cout << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': std::cout << "\\\\"; break;
      case '"': std::cout << "\\\""; break;
      case '\n': std::cout << "\\n"; break;
      case '\r': std::cout << "\\r"; break;
      case '\t': std::cout << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          std::cout << "\\u00" << kHex[(ch >> 4) & 0xf] << kHex[ch & 0xf];
        } else {
          std::cout << static_cast<char>(ch);
        }
    }
  }
  std::cout << '"';
}

struct PlanScan {
  size_t containers = 0;
  size_t unchanged = 0;
  size_t ptx_retargeted = 0;
  size_t native_cubin_retargeted = 0;
  size_t rejected = 0;
  size_t external_storage = 0;
  std::string first_rejection;
};

PlanScan ScanPlanner(const p::internal::Image& image,
                     const mfgunlock::ArchitectureProfile& profile) {
  PlanScan scan;
  for (const auto& section : image.sections) {
    if (!(section.Characteristics & IMAGE_SCN_MEM_READ) ||
        (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
      continue;
    }
    auto* start = image.base + section.VirtualAddress;
    const size_t size = section.Misc.VirtualSize;
    if (!p::internal::IsReadable(start, size, reinterpret_cast<HMODULE>(image.base))) {
      if (scan.first_rejection.empty()) scan.first_rejection = "unreadable data section";
      ++scan.rejected;
      continue;
    }
    for (size_t at = 0; at + f::kHeaderBytes <= size;) {
      if (f::ReadU32(start + at) != f::kMagic) {
        ++at;
        continue;
      }
      const uint64_t payload = f::ReadU64(start + at + 8);
      if (payload > size - at - f::kHeaderBytes ||
          payload > f::kMaxFatbinBytes - f::kHeaderBytes) {
        if (scan.first_rejection.empty()) scan.first_rejection = "fatbin outside section";
        ++scan.rejected;
        break;
      }
      const size_t bytes = static_cast<size_t>(payload) + f::kHeaderBytes;
      ++scan.containers;
      mfgunlock::ptx::Plan plan;
      std::string reason;
      auto result = mfgunlock::ptx::Retarget({start + at, bytes}, plan, reason, profile);
      if (result == mfgunlock::ptx::Result::kRetargeted) {
        ++scan.ptx_retargeted;
        scan.external_storage += plan.requires_external_storage;
      } else if (result == mfgunlock::ptx::Result::kRejected &&
                 profile.architecture == mfgunlock::Architecture::kAmpere &&
                 profile.target_sm == 86) {
        std::string native_reason;
        if (mfgunlock::cubin::RetargetOpaqueSm89To86(
                {start + at, bytes}, plan.replacement, native_reason)) {
          ++scan.native_cubin_retargeted;
          result = mfgunlock::ptx::Result::kRetargeted;
        }
      }
      if (result == mfgunlock::ptx::Result::kRejected) {
        ++scan.rejected;
        if (scan.first_rejection.empty()) scan.first_rejection = reason;
      } else if (result == mfgunlock::ptx::Result::kUnchanged) {
        ++scan.unchanged;
      }
      at += bytes;
    }
  }
  return scan;
}

std::vector<std::string> ExportPreparedPtx(const p::internal::Image& image,
                                         unsigned target, const std::filesystem::path& out) {
  if (std::filesystem::exists(out) && !std::filesystem::is_empty(out))
    throw std::runtime_error("PTX export directory must be new/empty");
  std::filesystem::create_directories(out);
  std::vector<std::string> files;
  std::vector<const p::internal::PayloadStorage*> redirected;
  for(const auto& provider:p::internal::g_providers)
    if(provider.module==image.base)for(const auto& storage:provider.overflow_storage)redirected.push_back(&storage);
  const auto emit=[&](const unsigned char* container,size_t total,uint32_t rva) {
    std::vector<f::Entry> entries;size_t end=0;
    if(!f::Parse({container,total},entries,end))throw std::runtime_error("PTX export parse");
    for(const auto& entry:entries) {
      if(entry.kind!=1 || entry.architecture!=target)continue;
      std::vector<unsigned char> raw;
      if(entry.compressed_bytes) {
        raw.resize(entry.unpacked_bytes);
        if(!f::Lz4BlockDecompress(container+entry.PayloadOffset(),entry.compressed_bytes,raw.data(),raw.size()))
          throw std::runtime_error("PTX export decompression");
      }else raw.assign(container+entry.PayloadOffset(),container+entry.End());
      const auto nul=std::find(raw.begin(),raw.end(),0);
      if(nul==raw.end())throw std::runtime_error("PTX export terminator");
      const std::string name=std::to_string(files.size())+"_rva_"+std::to_string(rva)+".ptx";
      std::ofstream file(out/name,std::ios::binary);
      file.write(reinterpret_cast<const char*>(raw.data()),std::distance(raw.begin(),nul));
      if(!file)throw std::runtime_error("PTX export write failed");
      files.push_back(name);
    }
  };
  for (const auto& section : image.sections) {
    if (!(section.Characteristics & IMAGE_SCN_MEM_READ) ||
        (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    auto* start=image.base+section.VirtualAddress;const size_t size=section.Misc.VirtualSize;
    for(size_t at=0;at+f::kHeaderBytes<=size;) {
      const auto external=std::find_if(redirected.begin(),redirected.end(),[&](const auto* r){return r->original==start+at;});
      if(external!=redirected.end()) {
        emit(static_cast<const unsigned char*>((*external)->allocation),(*external)->allocation_bytes,section.VirtualAddress+static_cast<uint32_t>(at));
        at+=(*external)->original_bytes;continue;
      }
      if(f::ReadU32(start+at)!=f::kMagic){++at;continue;}
      const uint64_t n=f::ReadU64(start+at+8);
      if(n>size-at-f::kHeaderBytes || n>f::kMaxFatbinBytes-f::kHeaderBytes)
        throw std::runtime_error("PTX export container bounds");
      const size_t total=static_cast<size_t>(n)+f::kHeaderBytes;
      emit(start+at,total,section.VirtualAddress+static_cast<uint32_t>(at));
      at+=total;
    }
  }
  return files;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if ((argc != 3 && argc != 5) || std::string_view(argv[1]) != "--provider" ||
        (argc == 5 && std::string_view(argv[3]) != "--emit-ptx"))
      throw std::runtime_error("usage: probe_provider_capability --provider Ampere|Turing [--emit-ptx directory]");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const auto architecture = ParseArchitecture(argv[2]);
    std::vector<unsigned char> bytes;
    std::array<char, 65536> block{};
    while (std::cin) {
      std::cin.read(block.data(), block.size());
      const size_t n = static_cast<size_t>(std::cin.gcount());
      if (bytes.size() + n > 128u * 1024u * 1024u)
        throw std::runtime_error("input limit");
      bytes.insert(bytes.end(), block.data(), block.data() + n);
    }
    if (bytes.empty()) throw std::runtime_error("empty mapped image");

    const auto module = static_cast<HMODULE>(bytes.data());
    provider_mock::regions = {{module, bytes.size()}};
    provider_mock::paths[module] = "read-only-mapped-copy.dll";

    p::internal::Image image;
    if (!p::internal::InspectImage(module, image) || image.bytes != bytes.size())
      throw std::runtime_error("invalid mapped image");

    const size_t relocations=mfgunlock::validation::RelocateMappedProvider(bytes);
    const auto nt = f::ReadU32(bytes.data() + 60);
    const auto directory_rva = f::ReadU32(bytes.data() + nt + 24 + 112);
    const auto directory_bytes = f::ReadU32(bytes.data() + nt + 24 + 116);
    const auto fits = [&](size_t at, size_t size) {
      return at <= bytes.size() && size <= bytes.size() - at;
    };
    if (!fits(directory_rva, directory_bytes) || directory_bytes < 40)
      throw std::runtime_error("export bounds");
    const auto* exports = bytes.data() + directory_rva;
    const auto nfunc = f::ReadU32(exports + 20);
    const auto nname = f::ReadU32(exports + 24);
    const auto funcs = f::ReadU32(exports + 28);
    const auto names = f::ReadU32(exports + 32);
    const auto ords = f::ReadU32(exports + 36);
    if (nname > 8192 || nfunc > 8192 ||
        !fits(funcs, size_t{nfunc} * 4) ||
        !fits(names, size_t{nname} * 4) ||
        !fits(ords, size_t{nname} * 2)) {
      throw std::runtime_error("export table bounds");
    }
    size_t bound = 0;
    for (size_t i = 0; i < nname; ++i) {
      const auto name_rva = f::ReadU32(bytes.data() + names + 4 * i);
      const auto ordinal = f::ReadU16(bytes.data() + ords + 2 * i);
      if (ordinal >= nfunc || !fits(name_rva, 1))
        throw std::runtime_error("export ordinal/name");
      const size_t limit = std::min<size_t>(256, bytes.size() - name_rva);
      const auto* end = static_cast<const unsigned char*>(
          std::memchr(bytes.data() + name_rva, 0, limit));
      if (!end) throw std::runtime_error("export name limit");
      const auto rva = f::ReadU32(bytes.data() + funcs + ordinal * 4);
      if (!fits(rva, 1) ||
          (rva >= directory_rva && rva - directory_rva < directory_bytes)) {
        continue;
      }
      const std::string name(reinterpret_cast<const char*>(bytes.data() + name_rva),
                             static_cast<size_t>(end - (bytes.data() + name_rva)));
      provider_mock::exports[{module, name}] =
          reinterpret_cast<FARPROC>(bytes.data() + rva);
      ++bound;
    }

    mfgunlock::g_enabled = true;
    a::Configure(architecture);
    const auto* active = a::ActiveProfile();
    if (!active) throw std::runtime_error("architecture profile unavailable");

    mfgunlock::endpoint::StructuralPlan endpoint_plan;
    std::string endpoint_reason;
    const bool endpoint_identified = architecture == mfgunlock::Architecture::kTuring &&
        p::FindTuringEndpointPlan(module, endpoint_plan, endpoint_reason);
    const PlanScan plan = ScanPlanner(image, *active);
    const auto original = bytes;
    const bool identified = p::IsDlssgProvider(module);
    const bool prepared = p::PrepareProvider(module);
    const auto observation = p::InspectProvider(module);

    size_t changed = 0;
    for (size_t i = 0; i < bytes.size(); ++i) changed += bytes[i] != original[i];
    bool endpoint_selected=false;
    size_t endpoint_payloads=0;
    size_t external_storage=0,external_bytes=0,address_bytes_changed=0;
    size_t fatbins = 0;
    size_t hidden_cubins = 0;
    for (const auto& provider : p::internal::g_providers) {
      if (provider.module != module) continue;
      external_storage+=provider.overflow_storage.size();
      for(const auto& r:provider.overflow_storage) {
        external_bytes+=r.allocation_bytes;
        const auto offset=static_cast<size_t>(r.original-image.base);
        for(size_t i=0;i<8;++i)address_bytes_changed+=bytes[offset+i]!=original[offset+i];
      }
      endpoint_selected=provider.endpoint_selected;
      endpoint_payloads=provider.endpoint_payloads;
      fatbins += provider.fatbins;
      hidden_cubins += provider.hidden_cubins;
    }

    std::vector<unsigned char*> gates;
    const bool mfg_gates = p::FindQualifiedMfgGates(module, gates);
    std::string failure_reason;
    for (const auto& rejection : p::internal::g_rejected) {
      if (rejection.module == module) {
        failure_reason = rejection.reason;
        break;
      }
    }

    const size_t protection_before_repeat = provider_mock::protection_calls;
    const bool repeat_result = prepared ? p::PrepareProvider(module) : false;
    const bool idempotent = !prepared ||
        (repeat_result && protection_before_repeat == provider_mock::protection_calls);

    const auto exported = prepared && argc==5 ? ExportPreparedPtx(image,active->target_sm,argv[4])
                                              : std::vector<std::string>{};
    p::Restore();
    const bool rollback_identical = original == bytes && p::internal::g_providers.empty();

    std::cout << "{\"schema\":\"mfg_provider_capability_probe_sm75_t2\""
              << ",\"architecture\":";
    PrintJsonString(argv[2]);
#ifdef MFG_PROBE_SOURCE_ID
    std::cout << ",\"compiled_source_id\":\"" << MFG_PROBE_SOURCE_ID << "\"";
#else
    std::cout << ",\"compiled_source_id\":null";
#endif
    std::cout << ",\"identified\":" << (identified ? "true" : "false")
              << ",\"prepared\":" << (prepared ? "true" : "false")
              << ",\"endpoint_identified\":" << (endpoint_identified ? "true" : "false")
              << ",\"endpoint_selected\":" << (endpoint_selected ? "true" : "false")
              << ",\"endpoint_payloads\":" << endpoint_payloads
              << ",\"relocations_applied\":" << relocations
              << ",\"endpoint_reason\":";
    PrintJsonString(endpoint_reason);
    std::cout << ",\"endpoint_family\":";
    PrintJsonString(mfgunlock::endpoint::FamilyName(endpoint_plan.family));
    std::cout << ",\"endpoint_witnesses\":" << endpoint_plan.witnesses.size();
    std::cout << ",\"endpoint_plan\":{\"selectors\":[";
    if(endpoint_identified) for(size_t k=0;k<endpoint_plan.selectors.size();++k) {
      if(k)std::cout<<',';
      const auto& site=endpoint_plan.selectors[k];
      std::cout<<"{\"rva\":"<<site.rva<<",\"before\":["<<unsigned(site.before[0])<<','<<unsigned(site.before[1])
               <<"],\"after\":["<<unsigned(site.after[0])<<','<<unsigned(site.after[1])<<"]}";
    }
    std::cout<<"],\"loaders\":[";
    if(endpoint_identified) for(size_t k=0;k<endpoint_plan.loaders.size();++k) {
      if(k)std::cout<<',';
      const auto& loader=endpoint_plan.loaders[k];
      std::cout<<"{\"slot_rva\":"<<loader.rva<<",\"function_rva\":"<<loader.target_rva<<'}';
    }
    std::cout<<"],\"payloads\":[";
    for(size_t k=0;k<endpoint_plan.payloads.size();++k) {
      if(k)std::cout<<',';
      const auto& payload=endpoint_plan.payloads[k];
      std::cout<<"{\"rva\":"<<payload.rva<<",\"bytes\":"<<payload.bytes<<",\"entry\":";
      PrintJsonString(payload.entry_name);std::cout<<'}';
    }
    std::cout<<"],\"host_witness_ranges\":[";
    for(size_t k=0;k<endpoint_plan.witnesses.size();++k) {
      if(k)std::cout<<',';
      const auto& w=endpoint_plan.witnesses[k];
      std::cout<<"{\"rva\":"<<w.rva<<",\"bytes\":"<<w.bytes.size()<<'}';
    }
    std::cout<<"]}";

    std::cout << ",\"exports_bound\":" << bound
              << ",\"profile_known\":" << (observation.profile_known ? "true" : "false")
              << ",\"exact_inventory\":" << (observation.exact_inventory ? "true" : "false")
              << ",\"fatbins\":" << fatbins
              << ",\"hidden_cubins\":" << hidden_cubins
              << ",\"changed_bytes\":" << changed
              << ",\"changed_non_address_bytes\":" << changed-address_bytes_changed
              << ",\"address_cells\":" << external_storage
              << ",\"external_storage\":" << external_storage
              << ",\"external_storage_bytes\":" << external_bytes
              << ",\"mfg_gates_matched\":" << (mfg_gates ? "true" : "false")
              << ",\"mfg_gate_count\":" << gates.size()
              << ",\"planner\":{\"containers\":" << plan.containers
              << ",\"unchanged\":" << plan.unchanged
              << ",\"ptx_retargeted\":" << plan.ptx_retargeted
              << ",\"native_cubin_retargeted\":" << plan.native_cubin_retargeted
              << ",\"external_storage\":" << plan.external_storage
              << ",\"rejected\":" << plan.rejected
              << ",\"first_rejection\":";
    PrintJsonString(plan.first_rejection);
    std::cout << "},\"failure_reason\":";
    PrintJsonString(failure_reason);
    std::cout << ",\"ptx_files\":[";
    for (size_t i=0;i<exported.size();++i) { if(i)std::cout<<',';PrintJsonString(exported[i]); }
    std::cout << "],\"idempotent\":" << (idempotent ? "true" : "false")
              << ",\"rollback_identical\":" << (rollback_identical ? "true" : "false")
              << ",\"vendor_code_executed\":false"
              << ",\"gpu_execution\":\"NOT_RUN\"}\n";
    return idempotent && rollback_identical ? 0 : 3;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
