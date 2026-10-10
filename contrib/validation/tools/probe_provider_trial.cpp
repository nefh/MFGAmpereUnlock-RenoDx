// SPDX-License-Identifier: MIT
// Invoke production PrepareProvider on a data copy with explicit OS doubles.
// No LoadLibrary, provider function call, original file write or GPU execution.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
std::string Quote(const std::string& s) {
  std::string out="\"";
  for(unsigned char c:s) {
    if(c=='"'||c=='\\') {out+='\\';out+=static_cast<char>(c);}
    else if(c>=32 && c<127) out+=static_cast<char>(c);
    else out+='?';
  }
  return out+'"';
}
int main(int argc, char** argv) {
  try {
    if(argc!=4) throw std::runtime_error("usage: probe_provider_trial Ampere|Turing architecture-export-rva parameters-export-rva < mapped-image");
    const auto arch=mfgunlock::architecture::Parse(argv[1]);
    const auto* profile=mfgunlock::architecture::GetProfile(arch);
    if(!profile || !profile->RequiresProviderRetarget()) throw std::runtime_error("retarget architecture required");
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::vector<unsigned char> image;
    std::array<char,65536> chunk{};
    while(std::cin) {
      std::cin.read(chunk.data(),chunk.size()); const auto n=static_cast<size_t>(std::cin.gcount());
      if(image.size()+n>128u*1024u*1024u) throw std::runtime_error("image size budget exceeded");
      image.insert(image.end(),chunk.data(),chunk.data()+n);
    }
    if(image.size()<sizeof(IMAGE_NT_HEADERS64)) throw std::runtime_error("truncated image");
    const auto arch_rva=std::stoull(argv[2]), parameters_rva=std::stoull(argv[3]);
    if(arch_rva>=image.size() || parameters_rva>=image.size()) throw std::runtime_error("export outside image");
    const auto before=image;
    const auto module=static_cast<HMODULE>(image.data());
    provider_mock::regions={{module,image.size()}};
    provider_mock::paths[module]="offline-data-copy.dll";
    provider_mock::exports[{module,"NVSDK_NGX_GetGPUArchitecture"}]=reinterpret_cast<FARPROC>(image.data()+arch_rva);
    provider_mock::exports[{module,"NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"}]=reinterpret_cast<FARPROC>(image.data()+parameters_rva);
    mfgunlock::g_enabled=true; mfgunlock::architecture::Configure(arch);
    namespace provider=mfgunlock::provider;
    const bool prepared=provider::PrepareProvider(module);
    const auto observation=provider::InspectProvider(module);
    const auto ready=provider::PreparedProviderCount();
    size_t changed=0; for(size_t i=0;i<image.size();++i) changed+=image[i]!=before[i];
    unsigned fatbins=0, cubins=0;
    for(const auto& record:provider::internal::g_providers) {fatbins+=record.fatbins;cubins+=record.hidden_cubins;}
    std::vector<unsigned char*> gates;
    const bool gates_found=provider::FindQualifiedMfgGates(module,gates);
    std::string reason;
    for(const auto& rejection:provider::internal::g_rejected) reason=rejection.reason;
    const auto writes=provider_mock::protection_calls;
    const bool repeat=prepared && provider::PrepareProvider(module);
    const bool idempotent=!prepared || (repeat && provider_mock::protection_calls==writes);
    provider::Restore();
    const bool restored=image==before && provider::internal::g_providers.empty();
    std::cout<<"{\"scope\":\"production mapped transaction on data with OS doubles; NOT GPU execution\""
      <<",\"trial_prepared\":"<<(prepared?"true":"false")
      <<",\"prepared_providers\":"<<ready<<",\"exact_registry_evidence\":"<<(observation.exact_inventory?"true":"false")
      <<",\"fatbins\":"<<fatbins<<",\"hidden_cubins\":"<<cubins<<",\"changed_copy_bytes\":"<<changed
      <<",\"mfg_contexts_matched\":"<<(gates_found?"true":"false")<<",\"mfg_context_count\":"<<gates.size()
      <<",\"failure_reason\":"<<Quote(reason)<<",\"idempotent\":"<<(idempotent?"true":"false")
      <<",\"restore_byte_identical\":"<<(restored?"true":"false")
      <<",\"original_file_modified\":false,\"gpu_execution\":\"NOT_RUN\"}\n";
    return restored&&idempotent?0:2;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
