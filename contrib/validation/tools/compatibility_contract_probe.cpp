// SPDX-License-Identifier: MIT
// Read-only oracle for local corpus evidence. No DLL is loaded or called.
#include "../../../src/addons/mfgunlock/compatibility_runtime.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace c = mfgunlock::compatibility;
unsigned Number(const char* text) {
  unsigned value = 0;
  const std::string_view input(text);
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
    throw std::runtime_error("invalid numeric argument");
  return value;
}
int main(int argc, char** argv) {
  try {
    if (argc >= 5 && std::string_view(argv[1]) == "--contract") {
      std::vector<c::ExportSymbol> symbols;
      for (int i = 5; i < argc; ++i) symbols.push_back({argv[i], true});
      (void)Number(argv[2]);  // File-version metadata; not an adapter input.
      const auto result = c::AssessInterposer(symbols, Number(argv[3]) != 0,
                                              Number(argv[4]) != 0);
      std::cout << "{\"surface\":\"" << c::Name(result.surface)
                << "\",\"typed_abi\":" << static_cast<unsigned>(result.typed_abi)
                << ",\"admission\":\"" << c::Name(result.admission.state)
                << "\",\"reason\":\"" << result.admission.reason
                << "\",\"runtime_tested\":false}\n";
      return 0;
    }
    if (argc == 7 && std::string_view(argv[1]) == "--gateway") {
      (void)Number(argv[2]);  // Parent version is not a gateway prerequisite.
      c::PluginGatewayEvidence evidence{};
      (void)Number(argv[3]);  // File-version metadata only.
      evidence.named_dlssg = Number(argv[4]) != 0;
      evidence.own_code_gateway = Number(argv[5]) != 0;
      evidence.current_image = Number(argv[6]) != 0;
      const auto result = c::GatewayLookup(evidence);
      std::cout << "{\"gateway\":\"" << c::Name(result.state)
                << "\",\"reason\":\"" << result.reason
                << "\",\"plugin_functions\":\"RUNTIME_UNOBSERVED\",\"runtime_tested\":false}\n";
      return 0;
    }
    throw std::runtime_error("usage: --contract major common current [exports...] OR --gateway parent-adapter plugin-major named own current");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
