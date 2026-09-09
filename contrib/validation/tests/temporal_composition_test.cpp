// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/ampere_policy.hpp"
#include "../../../src/addons/mfgunlock/ampere_ptx.hpp"
#include "../tools/temporal_probe.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fb = mfgunlock::fatbin;
namespace ap = mfgunlock::ampere::ptx;
namespace temporal = mfgunlock::midpoint::internal;

using Bytes = std::vector<unsigned char>;

namespace {

unsigned int g_assertions = 0;

void Check(bool test) {
  if (!test) {
    throw std::runtime_error("assertion " + std::to_string(g_assertions));
  }
  ++g_assertions;
}

template <class T>
void Put(Bytes& bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

Bytes Container(const Bytes& raw) {
  // Literal-only LZ4 is a valid independent test encoding, not the runtime
  // retargeter.
  Bytes packed{0xF0};
  size_t extra = raw.size() - 15;
  for (; extra >= 255; extra -= 255) packed.push_back(255);
  packed.push_back(static_cast<unsigned char>(extra));
  packed.insert(packed.end(), raw.begin(), raw.end());

  Bytes fatbin(80 + ((packed.size() + 7) & ~size_t{7}));
  Put<uint32_t>(fatbin, 0, fb::kMagic);
  Put<uint16_t>(fatbin, 4, 1);
  Put<uint16_t>(fatbin, 6, 16);
  Put<uint64_t>(fatbin, 8, fatbin.size() - 16);
  Put<uint16_t>(fatbin, 16, 1);
  Put<uint16_t>(fatbin, 18, 0x101);
  Put<uint32_t>(fatbin, 20, 64);
  Put<uint64_t>(fatbin, 24, fatbin.size() - 80);
  Put<uint32_t>(fatbin, 32, static_cast<uint32_t>(packed.size()));
  Put<uint32_t>(fatbin, 44, 89);
  Put<uint64_t>(fatbin, 56, 0x2041);
  Put<uint64_t>(fatbin, 72, raw.size());
  std::copy(packed.begin(), packed.end(), fatbin.begin() + 80);
  return fatbin;
}

Bytes Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(std::string("cannot read ") + path);
  return {std::istreambuf_iterator<char>(file), {}};
}

void Composition(const Bytes& raw, bool mixed = false) {
  auto fatbin = Container(raw);
  if (mixed) {
    auto blackwell = fatbin;
    Put<uint32_t>(blackwell, 44, 120);

    Bytes cubin(64);
    Put<uint16_t>(cubin, 0, 2);
    Put<uint16_t>(cubin, 2, 0x101);
    Put<uint32_t>(cubin, 4, 64);
    Put<uint32_t>(cubin, 28, 89);

    blackwell.insert(blackwell.end(), fatbin.begin() + 16, fatbin.end());
    blackwell.insert(blackwell.end(), cubin.begin(), cubin.end());
    Put<uint64_t>(blackwell, 8, blackwell.size() - 16);
    fatbin = std::move(blackwell);
  }

  const auto* profile = temporal::FindTemporalProfile(fatbin.data(), fatbin.size());
  Check(profile != nullptr);

  Bytes ada;
  std::string reason;
  Check(temporal::BuildTemporalFatbin(fatbin.data(), fatbin.size(), *profile, ada, reason));

  ap::Plan ampere;
  Check(ap::Retarget(fatbin, ampere, reason) == ap::Result::kRetargeted);

  Bytes composed;
  const size_t visible_bytes =
      16 + static_cast<size_t>(fb::ReadU64(ampere.replacement.data() + 8));
  Check(temporal::BuildTemporalFatbin(ampere.replacement.data(), visible_bytes,
                                     *profile, composed, reason));
  Check(composed.size() == ada.size());

  std::vector<fb::Entry> entries;
  size_t end = 0;
  Check(fb::Parse(composed, entries, end));
  Check(entries.size() == (mixed ? 2u : 1u) && entries.back().architecture == 86 &&
        entries.back().flags == 0x41);

  const auto offset = entries.back().PayloadOffset();
  std::string ptx(reinterpret_cast<char*>(composed.data() + offset),
                  entries.back().payload_bytes);
  Check(ptx.find("ld.param.f32 %f134, [" + std::string(profile->entry_name) +
                 "_param_0+32]") != std::string::npos);
  Check(ptx.find(".target sm_86") != std::string::npos);

  // Restore the two architecture bytes. The remaining output must be exactly
  // the upstream temporal transformation.
  const auto target = ptx.find(".target sm_86") + std::string(".target sm_8").size();
  composed[offset + target] = '9';
  Put<uint32_t>(composed, entries.back().offset + 28, 89);
  Check(composed == ada);
}

void Policy() {
  using namespace mfgunlock::ampere;

  RequirementsEvidence evidence{true, true, 1, kNgxSuccess, kDlssGFeatureId,
                                kAdapterUnsupported, kAdaArchitecture};
  Check(CanRelaxRequirements(evidence));

  auto test = evidence;
  test.prepared_providers = 0;
  Check(!CanRelaxRequirements(test));
  test = evidence;
  test.prepared_providers = 2;
  Check(!CanRelaxRequirements(test));
  test = evidence;
  test.ampere_adapter = false;
  Check(!CanRelaxRequirements(test));
  test = evidence;
  test.enabled = false;
  Check(!CanRelaxRequirements(test));
  test = evidence;
  test.call_result = 0xbad00001;
  Check(!CanRelaxRequirements(test));
  test = evidence;
  test.feature = 1;
  Check(!CanRelaxRequirements(test));
  test = evidence;
  test.minimum_architecture = 0x1b0;
  Check(!CanRelaxRequirements(test));

  for (auto flags : {1u, 2u, 8u, 16u, 6u, 12u, 0xffffffffu}) {
    test = evidence;
    test.flags = flags;
    Check(!CanRelaxRequirements(test));
  }

  test = evidence;
  test.flags = 0;
  Check(CanRelaxRequirements(test));
}

Bytes Synthetic() {
  std::string ptx =
      ".version 8.7\n"
      ".target sm_89\n"
      ".address_size 64\n"
      ".visible .entry main_kernel(\n"
      ".param .align 8 .b8 main_kernel_param_0[144]\n"
      "){\n"
      ".reg .f32 %f<1362>;\n"
      "$L__BB0_3:\n";
  for (unsigned int i = 0; i < 104; ++i) {
    ptx += "mul.ftz.f32 %f0, %f1, 0f3F000000;\n";
  }
  ptx += "ret;\n}\n";
  ptx.resize(99361, ' ');
  ptx.push_back('\0');
  return {ptx.begin(), ptx.end()};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Policy();
    Composition(Synthetic());
    Composition(Synthetic(), true);

    if (argc == 3) {
      Composition(Read(argv[1]));
      Composition(Read(argv[1]), true);

      const auto old_provider = Container(Read(argv[2]));
      Check(temporal::FindTemporalProfile(old_provider.data(), old_provider.size()) == nullptr);

      Bytes output;
      std::string reason;
      Check(!temporal::BuildTemporalFatbin(old_provider.data(), old_provider.size(),
                                           temporal::kTemporalProfiles[0], output, reason));
    } else if (argc != 1) {
      throw std::runtime_error("usage: temporal_composition_test [current.ptx old.ptx]");
    }

    std::cout << "assertions=" << g_assertions << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
