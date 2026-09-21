// SPDX-License-Identifier: MIT
// Canonical, machine-readable projection of the runtime registry. No DLL input.
#include "../../../src/addons/mfgunlock/provider_profile.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace pf = mfgunlock::profiles;
std::string Hex(uint64_t n) {
  std::ostringstream text;
  text << std::hex << std::setfill('0') << std::setw(16) << n;
  return text.str();
}
void Regions(std::span<const pf::Region> regions) {
  std::cout << '[';
  bool first = true;
  for (const auto& r : regions) {
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"rva\":" << r.rva << ",\"bytes\":" << r.bytes
              << ",\"fnv1a64\":\"" << Hex(r.hash) << "\"}";
  }
  std::cout << ']';
}
int main() {
  std::cout << "{\"schema\":1,\"profiles\":[";
  bool first = true;
  for (const auto& p : pf::kProfiles) {
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"id\":\"" << p.id << "\",\"version\":\"" << p.version
              << "\",\"sha256\":\"" << p.file_sha256 << "\",\"timestamp\":" << p.timestamp
              << ",\"image_size\":" << p.image_size << ",\"retarget_payloads\":";
    Regions(p.retarget_payloads);
    std::cout << ",\"endpoint_code\":"; Regions(p.endpoint_code);
    std::cout << ",\"endpoint_payloads\":"; Regions(p.endpoint_payloads);
    std::cout << ",\"selectors\":[";
    for (size_t i = 0; i < p.selectors.size(); ++i) {
      if (i) std::cout << ',';
      const auto& s = p.selectors[i];
      std::cout << "{\"rva\":" << s.rva << ",\"before\":[" << unsigned(s.before[0])
                << ',' << unsigned(s.before[1]) << "]}";
    }
    std::cout << "],\"endpoint_vtables\":[";
    for (size_t i = 0; i < p.endpoint_vtables.size(); ++i) {
      if (i) std::cout << ',';
      std::cout << "{\"rva\":" << p.endpoint_vtables[i].rva
                << ",\"target_rva\":" << p.endpoint_vtables[i].target_rva << '}';
    }
    std::cout << "],\"mfg_gates\":[";
    for (size_t i = 0; i < p.mfg_gates.size(); ++i) {
      if (i) std::cout << ',';
      const auto& g = p.mfg_gates[i];
      std::cout << "{\"rva\":" << g.code.rva << ",\"bytes\":" << g.code.bytes
                << ",\"fnv1a64\":\"" << Hex(g.code.hash) << "\",\"immediate_rva\":" << g.immediate_rva << '}';
    }
    std::cout << "],\"kernels\":[";
    for (size_t i = 0; i < p.quality_kernels.size(); ++i) {
      if (i) std::cout << ',';
      const auto& k = p.quality_kernels[i];
      const auto& x = *k.ptx;
      std::cout << "{\"role\":\"" << k.role << "\",\"entry\":\"" << x.entry_name
                << "\",\"signature\":\"" << x.parameter_signature << "\",\"arch\":" << x.arch
                << ",\"normalized_size\":" << x.normalized_size << ",\"declared_raw_size\":" << x.declared_raw_size
                << ",\"fnv1a64\":\"" << Hex(x.raw_fnv1a64) << "\",\"descriptor_references\":" << x.descriptor_references
                << ",\"ada_cubin_bytes\":" << k.ada_cubin_bytes << ",\"ada_cubin_fnv1a64\":\"" << Hex(k.ada_cubin_hash) << "\"}";
    }
    std::cout << "],\"backend_sms\":[" << p.backend_sms[0] << ',' << p.backend_sms[1] << ',' << p.backend_sms[2]
              << "],\"dynamic_streamline_version\":[" << p.dynamic_streamline_version[0] << ','
              << p.dynamic_streamline_version[1] << ',' << p.dynamic_streamline_version[2]
              << "],\"options_version\":" << p.options_version << ",\"state_version\":" << p.state_version
              << ",\"validated_generated_ceiling\":" << p.validated_generated_ceiling << '}';
  }
  std::cout << "]}\n";
}
