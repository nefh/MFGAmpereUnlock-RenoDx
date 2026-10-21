// SPDX-License-Identifier: MIT
// Read-only invocation of the production preset resolver on a mapped data copy.
// No provider instructions execute, no hooks install, no DLL is loaded.
#define MFG_TEST_STRICT_MODULE_HANDLES
#include "fg_preset.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

#ifndef MFG_PRESET_SOURCE_ID
#error Missing compiled source identity
#endif

namespace {
void Hex(std::span<const unsigned char> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  for (const auto byte : bytes) std::cout << digits[byte >> 4] << digits[byte & 15];
}
}

int main() {
  try {
    std::vector<unsigned char> image;
    std::array<char, 65536> buffer{};
    while (std::cin) {
      std::cin.read(buffer.data(), buffer.size());
      const auto count = static_cast<size_t>(std::cin.gcount());
      if (image.size() + count > 128u * 1024u * 1024u)
        throw std::runtime_error("mapped input exceeds inspection resource budget");
      image.insert(image.end(), buffer.data(), buffer.data() + count);
    }
    if (image.size() < sizeof(IMAGE_NT_HEADERS64)) throw std::runtime_error("truncated mapped image");
    const auto before = image;
    const auto module = reinterpret_cast<HMODULE>(image.data());
    provider_mock::regions.push_back({module, image.size(), MEM_IMAGE, PAGE_EXECUTE_READ});
    namespace preset = mfgunlock::fgpreset::internal;
    namespace gate = mfgunlock::mfggate;
    mfgunlock::provider::internal::Image pe;
    if (!mfgunlock::provider::internal::InspectImage(module, pe))
      throw std::runtime_error("invalid mapped PE image");
    std::vector<gate::Range> ranges;
    for (const auto& section : pe.sections)
      if (section.Misc.VirtualSize)
        ranges.push_back({section.VirtualAddress, section.Misc.VirtualSize,
                           (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    gate::View view{{pe.base, pe.bytes}, ranges, {}};
    const bool functions_read = view.ReadFunctions();
    preset::Layout layout;
    const bool resolved = preset::ResolveLayout(module, layout);
    std::cout << "{\"schema\":\"mfg_preset_layout_r6_3_1\",\"compiled_source_id\":\""
              << MFG_PRESET_SOURCE_ID << "\",\"scope\":\"OFFLINE_PRODUCTION_RESOLVER_WITH_OS_DOUBLES\""
              << ",\"vendor_code_executed\":false,\"hooks_installed\":false,\"runtime_tested\":false"
              << ",\"reader_resolved\":" << (resolved ? "true" : "false")
              << ",\"structural_reader\":" << (resolved && layout.structural ? "true" : "false")
              << ",\"reader_abi\":\"" << (!resolved ? "UNRESOLVED" : layout.cached_bool ? "BOOL_AL_CACHED" :
                  layout.structural ? "CANONICAL_U64_LEAF" : "BOOL_AL_REFERENCE") << "\""
              << ",\"observer_resolved\":" << (resolved && layout.observer_rva ? "true" : "false")
              << ",\"reader_rva\":" << (resolved ? layout.reader_rva : 0)
              << ",\"observer_rva\":" << (resolved ? layout.observer_rva : 0)
              << ",\"applied_offset\":" << (resolved ? layout.applied_offset : 0)
              << ",\"input_unchanged\":" << (image == before ? "true" : "false")
              << ",\"pdata_read\":" << (functions_read ? "true" : "false")
              << ",\"selection_returns\":[";
    if (resolved) for (size_t i = 0; i < layout.selection_returns.size(); ++i) {
      if (i) std::cout << ',';
      std::cout << layout.selection_returns[i];
    }
    std::cout << "],\"literal_candidates_are_not_abi_proof\":true,\"code_contexts\":[";
    // Export small complete function contexts for unresolved readers. Raw
    // literals only select evidence; never use them to authorize a live hook.
    constexpr std::array<unsigned char, 4> setting{0xf1, 0x1d, 0xe4, 0x10};
    std::set<std::pair<uint32_t, uint32_t>> contexts;
    size_t literal_candidates = 0;
    bool limited = false;
    struct GraphReport { uint32_t start, end; bool built; unsigned stopped; bool overlap; };
    std::vector<GraphReport> graph_reports;
    bool graphs_complete = functions_read;
    if (functions_read) for (const auto& function : view.functions) {
      if (function.first >= function.second || function.second > image.size()) continue;
      const auto first = image.begin() + function.first, last = image.begin() + function.second;
      if (std::search(first, last, setting.begin(), setting.end()) == last) continue;
      ++literal_candidates;
      if (contexts.size() >= 64) { limited = true; graphs_complete = false; continue; }
      contexts.insert(function);
      gate::Graph graph;
      const bool built = graph.Build(view, function);
      graph_reports.push_back({function.first, function.second, built, graph.stopped, graph.overlap});
      graphs_complete = graphs_complete && built && graph.stopped == 0 && !graph.overlap;
      // Reached, fully decoded calls are useful evidence even if ANOTHER path
      // stopped. Never scan across the undecoded gap or authorize a hook here.
      if (!built || graph.overlap) continue;
      for (const auto& [rva, instruction] : graph.instructions) {
        (void)rva;
        if (!instruction.call || instruction.op != 0xe8) continue;
        const auto callee = view.Function(instruction.target);
        if (callee.first != instruction.target || callee.first >= callee.second) continue;
        if (contexts.size() < 64) contexts.insert(callee); else limited = true;
      }
    }
    if (resolved) for (const auto& witness : layout.witnesses) {
      if (witness.bytes.empty() || witness.bytes.size() > UINT32_MAX - witness.rva) continue;
      const auto range = std::pair{witness.rva, witness.rva + static_cast<uint32_t>(witness.bytes.size())};
      if (contexts.contains(range)) continue;
      if (contexts.size() < 64) contexts.insert(range); else limited = true;
    }
    size_t emitted = 0, total_bytes = 0;
    for (const auto& [first, last] : contexts) {
      const size_t bytes = static_cast<size_t>(last - first);
      if (bytes > 65536 || last > image.size() || total_bytes + bytes > 1024 * 1024) {
        limited = true;
        continue;
      }
      if (emitted++) std::cout << ',';
      total_bytes += bytes;
      std::cout << "{\"start_rva\":" << first << ",\"end_rva\":" << last
                << ",\"complete_function\":true,\"hex\":\"";
      Hex(std::span<const unsigned char>(image).subspan(first, bytes));
      std::cout << "\"}";
    }
    std::cout << "],\"literal_candidate_count\":" << literal_candidates
              << ",\"contexts_limited\":" << (limited ? "true" : "false")
              << ",\"contexts_exhaustive\":false,\"candidate_graphs_complete\":"
              << (graphs_complete && literal_candidates != 0 ? "true" : "false")
              << ",\"candidate_graphs\":[";
    for (size_t i = 0; i < graph_reports.size(); ++i) {
      if (i) std::cout << ',';
      const auto& graph = graph_reports[i];
      std::cout << "{\"start_rva\":" << graph.start << ",\"end_rva\":" << graph.end
                << ",\"built\":" << (graph.built ? "true" : "false")
                << ",\"stopped_paths\":" << graph.stopped
                << ",\"overlap\":" << (graph.overlap ? "true" : "false") << '}';
    }
    std::cout << "]}\n";
    return image == before ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
