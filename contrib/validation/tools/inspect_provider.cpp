// SPDX-License-Identifier: MIT
// Read-only provider inspection. The file is parsed as data; it is never loaded
// and no CUDA code is executed.

#include "../../../src/addons/mfgunlock/ampere_ptx.hpp"
#include "temporal_probe.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fatbin = mfgunlock::fatbin;
namespace ampere_ptx = mfgunlock::ampere::ptx;
namespace temporal = mfgunlock::midpoint::internal;

using Bytes = std::vector<unsigned char>;

namespace {

std::string Quoted(const std::string& value) {
  std::string output = "\"";
  for (unsigned char character : value) {
    if (character == '\\' || character == '"') {
      output += '\\';
      output += static_cast<char>(character);
    } else if (character >= 32 && character < 127) {
      output += static_cast<char>(character);
    } else {
      output += '?';
    }
  }
  output += '"';
  return output;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2 && argc != 3) {
      throw std::runtime_error("usage: inspect_provider <nvngx_dlssg.dll or .fatbin> [Ampere|Turing|Ada]");
    }
    const auto selected = mfgunlock::architecture::Parse(argc == 3 ? argv[2] : nullptr);
    const auto* target_profile = mfgunlock::architecture::GetProfile(selected);
    if (!target_profile) throw std::runtime_error("offline inspection requires an explicit architecture");

    const auto path = std::filesystem::path(argv[1]);
    const auto file_size = std::filesystem::file_size(path);
    if (file_size < 16 || file_size > 512 * 1024 * 1024) {
      throw std::runtime_error("invalid file size");
    }

    std::ifstream input(path, std::ios::binary);
    Bytes data{std::istreambuf_iterator<char>(input), {}};
    if (data.size() != file_size) throw std::runtime_error("short read");

    const auto in_range = [file_size](uint64_t offset, uint64_t bytes) {
      return offset <= file_size && bytes <= file_size - offset;
    };
    const auto read_u16 = [&](size_t offset) {
      if (!in_range(offset, 2)) throw std::runtime_error("PE u16 bounds");
      return fatbin::ReadU16(data.data() + offset);
    };
    const auto read_u32 = [&](size_t offset) {
      if (!in_range(offset, 4)) throw std::runtime_error("PE u32 bounds");
      return fatbin::ReadU32(data.data() + offset);
    };

    std::vector<std::pair<size_t, size_t>> sections;
    if (read_u32(0) == fatbin::kMagic) {
      sections.emplace_back(0, data.size());
    } else {
      if (read_u16(0) != 0x5a4d || !in_range(0x3c, 4)) {
        throw std::runtime_error("not PE or fatbin");
      }

      const size_t pe_offset = read_u32(0x3c);
      if (!in_range(pe_offset, 24) || read_u32(pe_offset) != 0x4550 ||
          read_u16(pe_offset + 4) != 0x8664) {
        throw std::runtime_error("not x64 PE");
      }

      const size_t section_count = read_u16(pe_offset + 6);
      const size_t optional_size = read_u16(pe_offset + 20);
      const size_t section_table = pe_offset + 24 + optional_size;
      if (optional_size < 112 || read_u16(pe_offset + 24) != 0x20b ||
          section_count == 0 || section_count > 96 ||
          !in_range(section_table, 40 * section_count)) {
        throw std::runtime_error("invalid PE headers");
      }

      for (size_t index = 0; index < section_count; ++index) {
        const size_t section = section_table + 40 * index;
        const size_t bytes = read_u32(section + 16);
        const size_t offset = read_u32(section + 20);
        if (bytes != 0 && !in_range(offset, bytes)) {
          throw std::runtime_error("section outside file");
        }

        const auto flags = read_u32(section + 36);
        const bool readable = (flags & 0x40000000) != 0;
        const bool executable = (flags & 0x20000000) != 0;
        if (bytes != 0 && readable && !executable) sections.emplace_back(offset, bytes);
      }

      std::sort(sections.begin(), sections.end());
      for (size_t index = 1; index < sections.size(); ++index) {
        const auto& previous = sections[index - 1];
        if (sections[index].first < previous.first + previous.second) {
          throw std::runtime_error("overlapping sections");
        }
      }
    }

    size_t retargeted = 0;
    size_t rejected = 0;
    size_t temporal_matches = 0;
    size_t total = 0;

    std::cout << "{\n"
              << "  \"schema\":\"mfgampereunlock_provider_inspection_v1\",\n"
              << "  \"file\":" << Quoted(path.filename().string()) << ",\n"
              << "  \"architecture\":" << Quoted(mfgunlock::architecture::Name(selected)) << ",\n"
              << "  \"target_sm\":" << target_profile->target_sm << ",\n"
              << "  \"fatbins\":[\n";

    for (auto [section_offset, section_bytes] : sections) {
      for (size_t offset = 0; offset + 16 <= section_bytes;) {
        const auto* candidate = data.data() + section_offset + offset;
        if (fatbin::ReadU32(candidate) != fatbin::kMagic) {
          ++offset;
          continue;
        }

        const auto payload_bytes = fatbin::ReadU64(candidate + 8);
        if (payload_bytes > section_bytes - offset - 16 ||
            payload_bytes > fatbin::kMaxFatbinBytes - 16) {
          throw std::runtime_error("invalid fatbin bounds");
        }

        const size_t fatbin_bytes = 16 + static_cast<size_t>(payload_bytes);
        if (++total > 512) throw std::runtime_error("fatbin count limit");

        ampere_ptx::Plan plan;
        std::string detail;
        const auto result = ampere_ptx::Retarget({candidate, fatbin_bytes}, plan, detail, *target_profile);
        const bool accepted = result == ampere_ptx::Result::kRetargeted;
        retargeted += accepted;
        rejected += result == ampere_ptx::Result::kRejected;

        bool temporal_compatible = false;
        if (accepted || !target_profile->NeedsRetarget()) {
          const auto* prepared = accepted ? plan.replacement.data() : candidate;
          const size_t replacement_bytes =
              16 + static_cast<size_t>(fatbin::ReadU64(prepared + 8));
          const auto* profile = temporal::FindTemporalProfile(prepared, replacement_bytes);
          Bytes output;
          if (profile != nullptr) {
            temporal_compatible = temporal::BuildTemporalFatbin(
                prepared, replacement_bytes, *profile, output, detail);
          }
        }
        temporal_matches += temporal_compatible;

        if (total > 1) std::cout << ",\n";
        std::cout << "    {\"offset\":" << section_offset + offset
                  << ",\"bytes\":" << fatbin_bytes
                  << ",\"retargetable\":" << (accepted ? "true" : "false")
                  << ",\"temporal_profile\":"
                  << (temporal_compatible ? "true" : "false")
                  << ",\"detail\":" << Quoted(detail) << '}';

        offset += fatbin_bytes;
      }
    }

    const bool candidate = (!target_profile->NeedsRetarget() || retargeted > 0) &&
                           rejected == 0 && temporal_matches == 1;
    std::cout << "\n  ],\n"
              << "  \"retargetable\":" << retargeted << ",\n"
              << "  \"rejected\":" << rejected << ",\n"
              << "  \"temporal_matches\":" << temporal_matches << ",\n"
              << "  \"offline_mfg_candidate\":" << (candidate ? "true" : "false")
              << ",\n"
              << "  \"descriptor_binding\":\"NOT_TESTED\",\n"
              << "  \"cuda_execution\":\"NOT_TESTED\",\n"
              << "  \"native_mfg\":\"NOT_TESTED\"\n"
              << "}\n";

    return candidate ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
