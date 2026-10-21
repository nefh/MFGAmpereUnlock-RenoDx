// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/capability_policy.hpp"
#include "../../../src/addons/mfgunlock/ptx_retarget.hpp"
#if defined(MFG_TEST_RUNTIME_MIDPOINT)
#include "../../../src/addons/mfgunlock/midpoint.hpp"
#else
#include "../tools/temporal_probe.hpp"
#endif

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fb = mfgunlock::fatbin;
namespace ptx = mfgunlock::ptx;
namespace temporal = mfgunlock::midpoint::internal;
using Bytes = std::vector<unsigned char>;

namespace {
unsigned int g_assertions = 0;
void Check(bool test, const char* reason = "check") {
  ++g_assertions;
  if (!test) throw std::runtime_error(std::string(reason) + " (assertion " + std::to_string(g_assertions) + ")");
}
template <class T> void Put(Bytes& bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
Bytes Raw(std::string text) {
  text.push_back('\0');
  return {text.begin(), text.end()};
}
Bytes Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(std::string("cannot read ") + path);
  return {std::istreambuf_iterator<char>(file), {}};
}
Bytes Container(const Bytes& raw, bool compressed = true, uint32_t architecture = 89,
                size_t header = 64, uint64_t flags = 0x41) {
  // Independent literal-only LZ4 encoding, including the short literal case.
  Bytes packed;
  if (compressed) {
    packed.push_back(static_cast<unsigned char>(std::min<size_t>(raw.size(), 15) << 4));
    if (raw.size() >= 15) {
      size_t extra = raw.size() - 15;
      for (; extra >= 255; extra -= 255) packed.push_back(255);
      packed.push_back(static_cast<unsigned char>(extra));
    }
    packed.insert(packed.end(), raw.begin(), raw.end());
  } else packed = raw;
  Bytes fatbin(16 + header + ((packed.size() + 7) & ~size_t{7}));
  Put<uint32_t>(fatbin, 0, fb::kMagic);
  Put<uint16_t>(fatbin, 4, 1); Put<uint16_t>(fatbin, 6, 16);
  Put<uint64_t>(fatbin, 8, fatbin.size() - 16);
  Put<uint16_t>(fatbin, 16, 1); Put<uint16_t>(fatbin, 18, 0x101);
  Put<uint32_t>(fatbin, 20, static_cast<uint32_t>(header));
  Put<uint64_t>(fatbin, 24, fatbin.size() - 16 - header);
  Put<uint32_t>(fatbin, 32, compressed ? static_cast<uint32_t>(packed.size()) : 0);
  Put<uint32_t>(fatbin, 44, architecture);
  Put<uint64_t>(fatbin, 56, compressed ? flags | 0x2000 : flags & ~uint64_t{0x2000});
  Put<uint64_t>(fatbin, 72, compressed ? raw.size() : 0);
  std::fill(fatbin.begin() + 80, fatbin.begin() + 16 + header, 0xa5);
  std::copy(packed.begin(), packed.end(), fatbin.begin() + 16 + header);
  return fatbin;
}
void AppendEntry(Bytes& fat, const Bytes& extra) {
  fat.insert(fat.end(), extra.begin() + 16, extra.end());
  Put<uint64_t>(fat, 8, fat.size() - 16);
}
std::string Decoded(const Bytes& fat, size_t index = 0) {
  std::vector<fb::Entry> entries; size_t end = 0;
  Check(fb::Parse(fat, entries, end) && index < entries.size(), "output container parses");
  Bytes raw; Check(fb::DecodePtx(fat, entries[index], raw), "output PTX decodes");
  return {raw.begin(), std::find(raw.begin(), raw.end(), 0)};
}
std::string Synthetic(std::string entry = "main_kernel", std::string parameter = "main_kernel_param_0",
                      uint32_t architecture = 89, unsigned count = 104, unsigned registers = 1362,
                      std::string label = "$L__BB0_3", bool variation = false) {
  std::string out = ".version 8.7\n.target sm_" + std::to_string(architecture) +
      "\n.address_size 64\n.visible .entry " + entry + "(\n.param .align 8 .b8 " +
      parameter + "[144]\n){\n.reg .f32 %f<" + std::to_string(registers) + ">;\n.reg .pred %p<1>;\n";
  if (variation) {
    out += ".reg .f32 %mfg_mid_0_t, %mfg_mid_1_inverse;\n"
           "mov.f32 %f134, 0f3F800000;\nmov.f32 %f135, %f134;\nmov.f32 %f136, %f135;\n"
           "@%p0 bra " + label + ";\n"
           "/* .entry main_kernel( .param .align 8 .b8 fake[144]){\n"
           "mul.ftz.f32 %fake, %fake, 0f3F000000; } */\n";
  }
  out += label + ":\n";
  for (unsigned i = 0; i < count; ++i) {
    if (variation && (i % 2))
      out += "@%p0 mul.ftz.f32\t%f0, /* operand */ 0f3F000000, %f1; // unchanged comment\n";
    else out += "mul.ftz.f32 %f0, %f1, 0f3F000000;\n";
  }
  out += "ret;\n}\n";
  if (variation) out += ".visible .entry unrelated() {\n.reg .f32 %other<2>;\n"
                        "mul.ftz.f32 %other0, %other1, 0f3F000000;\nret;\n}\n";
  return out;
}
size_t Count(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = 0; (at = text.find(needle, at)) != text.npos; at += needle.size()) ++count;
  return count;
}
void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
  for (size_t at = 0; (at = text.find(from, at)) != text.npos; at += to.size())
    text.replace(at, from.size(), to);
}
void EraseOne(std::string& text, const std::string& value) {
  Check(Count(text, value) == 1, "one complete injected statement block");
  text.erase(text.find(value), value.size());
}
void SemanticParity(const std::string& original, const Bytes& transformed,
                    const std::string& parameter, const std::string& prefix) {
  std::string patched = Decoded(transformed);
  const std::string nl = original.find("\r\n") == original.npos ? "\n" : "\r\n";
  const auto t = prefix + "_t", one = prefix + "_one", inverse = prefix + "_inverse";
  const std::string declarations = nl + "// MFGUNLOCK_MIDPOINT_R6" + nl +
      ".reg .f32 " + t + ", " + one + ", " + inverse + ";" + nl;
  const std::string initialize = "ld.param.f32 " + t + ", [" + parameter + "+32];" + nl +
      "mov.f32 " + one + ", 0f3F800000;" + nl +
      "sub.ftz.f32 " + inverse + ", " + one + ", " + t + ";" + nl;
  Check(patched.find(declarations) < patched.find(initialize), "fresh declarations precede initialization");
  if (original.find("@%p0 bra") != original.npos)
    Check(patched.find(initialize) < patched.find("@%p0 bra"), "initialization dominates the first branch");
  EraseOne(patched, declarations); EraseOne(patched, initialize);
  Check(Count(patched, inverse) == 52 && Count(patched, t) == 52, "two complementary 52-site groups");
  Check(patched.rfind(inverse) < patched.find(t), "established direction group order");
  ReplaceAll(patched, inverse, "0f3F000000"); ReplaceAll(patched, t, "0f3F000000");
  Check(patched == original, "all source outside the 104 weight operands is byte-preserved");
}
void StructuralVariants() {
  for (const auto& profile : temporal::kTemporalProfiles) {
    for (bool varied : {false, true}) {
      for (bool crlf : {false, true}) {
        std::string source = Synthetic(profile.entry_name, "actual_parameter_name", 89, 104,
            varied ? 241 : 1362, varied ? "renamed_branch_join" : "$L__BB0_3", varied);
        if (crlf) ReplaceAll(source, "\n", "\r\n");
        const auto encoded = Container(Raw(source));
        const auto uncompressed = Container(Raw(source), false);
        Check(temporal::FindTemporalProfile(encoded.data(), encoded.size()) == &profile,
              "semantic role discovery independent of payload size/register bank/label");
        Bytes one, two; std::string why;
        auto copied_profile = profile;
        Check(temporal::BuildTemporalFatbin(encoded.data(), encoded.size(), copied_profile, one, why),
              "copied role descriptor is not rejected by pointer identity");
        Check(temporal::BuildTemporalFatbin(uncompressed.data(), uncompressed.size(), profile, two, why),
              "raw PTX representation is accepted");
        Check(one == two, "compressed/raw transformations have identical bytes");
        SemanticParity(source, one, "actual_parameter_name", varied ? "%mfg_mid_2" : "%mfg_mid_0");
      }
    }
  }
  const auto source = Synthetic("main_kernel", "future_parameter", 999, 104, 2, "entry_path");
  auto unknown_sm = Container(Raw(source), true, 999, 344, 0x100041);
  Bytes output; std::string why;
  Check(temporal::BuildTemporalFatbin(unknown_sm.data(), unknown_sm.size(), temporal::kTemporalProfiles[0],
                                     output, why), "architecture is read from PTX, not an SM allowlist");
  SemanticParity(source, output, "future_parameter", "%mfg_mid_0");
  Check(fb::ReadU64(output.data() + 56) == 0x100041 && fb::ReadU32(output.data() + 20) == 344,
        "unrelated flags and extended header retained");
  Check(std::all_of(output.begin() + 80, output.begin() + 360, [](unsigned char b) { return b == 0xa5; }),
        "unknown header-extension bytes retained exactly");

  auto padded_source = Synthetic(); padded_source.resize(160000, ' ');
  auto padded = Container(Raw(padded_source));
  Check(temporal::BuildTemporalFatbin(padded.data(), padded.size(), temporal::kTemporalProfiles[0], output, why),
        "padding and size changes do not revoke operation");
  SemanticParity(padded_source, output, "main_kernel_param_0", "%mfg_mid_0");

  auto original = Container(Raw(Synthetic()));
  const auto unrelated = Container(Raw(".version 8.7\n.target sm_90\n.visible .entry another() {ret;}\n"), false, 90, 96);
  AppendEntry(original, unrelated);
  auto cubin = Container(Raw("unused binary"), false); Put<uint16_t>(cubin, 16, 2);
  AppendEntry(original, cubin);
  Check(temporal::BuildTemporalFatbin(original.data(), original.size(), temporal::kTemporalProfiles[0], output, why),
        "mixed container rebuild");
  std::vector<fb::Entry> entries; size_t end = 0;
  Check(fb::Parse(output, entries, end) && entries.size() == 2, "binary alternatives removed; PTX modules preserved");
  Check(Bytes(output.begin() + entries[1].offset, output.begin() + entries[1].End()) ==
        Bytes(unrelated.begin() + 16, unrelated.end()), "unrelated PTX entry header and payload unchanged");
}
void Rejected(const Bytes& input, const char* reason) {
  Bytes output{1, 2, 3}; std::string why;
  Check(temporal::FindTemporalProfile(input.data(), input.size()) == nullptr, reason);
  Check(!temporal::BuildTemporalFatbin(input.data(), input.size(), temporal::kTemporalProfiles[0], output, why), reason);
  Check(output.empty() && !why.empty(), "failure publishes no partial output");
}
void InvalidVariants() {
  for (unsigned count : {0u, 52u, 103u, 105u, 208u})
    Rejected(Container(Raw(Synthetic("main_kernel", "arg", 89, count))), "unproved direction group shape");
  for (const auto& edit : std::vector<std::pair<std::string, std::string>>{
      {"[144]", "[140]"}, {".align 8", ".align 3"},
      {"[144]", "[144], .param .u32 extra"}, {"ret;\n}", "ret;"},
      {".target sm_89", ".target sm_86"}, {".version 8.7", "/* .version 8.7"}}) {
    auto source = Synthetic(); ReplaceAll(source, edit.first, edit.second);
    Rejected(Container(Raw(source)), "malformed or unresolved parameter/body context");
  }
  auto source = Raw(Synthetic()); source.push_back('x');
  Rejected(Container(source), "nonzero data after PTX terminator");
  auto duplicate = Container(Raw(Synthetic())); const auto second = duplicate; AppendEntry(duplicate, second);
  Rejected(duplicate, "multiple genuine temporal modules are ambiguous");
  const auto valid = Container(Raw(Synthetic()));
  for (size_t size : {size_t{0}, size_t{15}, size_t{16}, size_t{79}, valid.size() - 1})
    Rejected(Bytes(valid.begin(), valid.begin() + size), "truncated container");
  auto bad = valid; Put<uint64_t>(bad, 8, UINT64_MAX); Rejected(bad, "outer size overflow");
  bad = valid; Put<uint32_t>(bad, 20, 8192); Rejected(bad, "entry header outside bounds");
  bad = valid; Put<uint32_t>(bad, 32, UINT32_MAX); Rejected(bad, "compressed stream outside entry");
  bad = valid; Put<uint64_t>(bad, 72, fb::kMaxPtxBytes + 1); Rejected(bad, "decoded PTX limit");
  bad = valid; bad[80] = 0; bad[81] = 0; bad[82] = 0; Rejected(bad, "invalid LZ4 back-reference");
}
void RuntimeBounds() {
#if defined(MFG_TEST_RUNTIME_MIDPOINT)
  Bytes image(128); auto* base = image.data();
  std::memcpy(base + 64, "main_kernel", 12);
  Put<uint64_t>(image, 48, reinterpret_cast<uintptr_t>(base + 64));
  provider_mock::regions.push_back({base, image.size(), MEM_IMAGE, PAGE_READONLY});
  uint64_t value = 0;
  Check(temporal::ReadRelativePointer(base, image.size(), base + 32, 16, value), "bounded descriptor field read");
  Check(temporal::PointsToCString(base, image.size(), value, "main_kernel"), "bounded descriptor role string");
  Check(!temporal::ReadRelativePointer(base, image.size(), base, -8, value), "negative offset stays inside image");
  Check(!temporal::ReadRelativePointer(base, image.size(), base + 120, 1, value), "partial pointer is rejected");
  Check(!temporal::PointsToCString(base, image.size(), reinterpret_cast<uintptr_t>(base + 123), "main_kernel"),
        "partial string is rejected");
  provider_mock::regions.back().protect = PAGE_NOACCESS;
  Check(!temporal::ReadRelativePointer(base, image.size(), base + 32, 16, value), "unreadable descriptor field is not dereferenced");
  Check(!temporal::PointsToCString(base, image.size(), reinterpret_cast<uintptr_t>(base + 64), "main_kernel"),
        "unreadable role string is not dereferenced");
  provider_mock::regions.pop_back();
#endif
}
void Composition(const Bytes& raw, bool mixed = false,
                 const mfgunlock::ArchitectureProfile& target_profile = mfgunlock::architecture::kAmpere) {
  auto fatbin = Container(raw);
  if (mixed) {
    auto other = fatbin; Put<uint32_t>(other, 44, 120); // mismatched metadata is not the selected module
    AppendEntry(other, fatbin);
    auto cubin = Container(Raw("unused cubin"), false); Put<uint16_t>(cubin, 16, 2);
    AppendEntry(other, cubin); fatbin = std::move(other);
  }
  const auto* profile = temporal::FindTemporalProfile(fatbin.data(), fatbin.size());
  Check(profile != nullptr, "temporal role exists before retarget");
  temporal::TemporalProgram program; std::string reason;
  Check(temporal::FindTemporalProgram(fatbin.data(), fatbin.size(), profile, program, reason));
  Bytes ada; Check(temporal::BuildTemporalFatbin(fatbin.data(), fatbin.size(), *profile, ada, reason));
  ptx::Plan retargeted;
  Check(ptx::Retarget(fatbin, retargeted, reason, target_profile) == ptx::Result::kRetargeted);
  Bytes composed;
  const size_t visible_bytes = 16 + static_cast<size_t>(fb::ReadU64(retargeted.replacement.data() + 8));
  Check(temporal::BuildTemporalFatbin(retargeted.replacement.data(), visible_bytes, *profile, composed, reason));
  Check(composed.size() == ada.size());
  std::vector<fb::Entry> entries; size_t end = 0;
  Check(fb::Parse(composed, entries, end));
  Check(entries.size() == (mixed ? 2u : 1u) && entries.back().architecture == target_profile.target_sm &&
        entries.back().flags == 0x41);
  const auto offset = entries.back().PayloadOffset();
  const std::string text(reinterpret_cast<char*>(composed.data() + offset), entries.back().payload_bytes);
  Check(text.find("ld.param.f32 %mfg_mid_0_t, [" + program.parameter + "+32]") != text.npos);
  const auto target_name = ".target sm_" + std::to_string(target_profile.target_sm);
  Check(text.find(target_name) != text.npos);
  // Restore only actual target metadata/text. All remaining bytes must compose
  // exactly with the transform run before architecture retargeting.
  const auto target = text.find(target_name) + std::string(".target sm_").size();
  composed[offset + target] = '8'; composed[offset + target + 1] = '9';
  Put<uint32_t>(composed, entries.back().offset + 28, 89);
  Check(composed == ada, "retarget and midpoint edits commute byte-for-byte");
}
void Policy() {
  using namespace mfgunlock::policy;

  RequirementsEvidence evidence{true, true, 1, kNgxSuccess, kDlssGFeatureId,
                                kAdapterUnsupported, kAdaArchitecture};
  Check(CanRelaxRequirements(evidence, &mfgunlock::architecture::kAmpere));

  auto test = evidence;
  test.prepared_providers = 0;
  Check(CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  test = evidence;
  test.prepared_providers = 2;
  Check(CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  test = evidence;
  test.adapter_bound = false;
  Check(!CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  test = evidence;
  test.enabled = false;
  Check(!CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  test = evidence;
  test.call_result = 0xbad00001;
  Check(!CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  test = evidence;
  test.feature = 1;
  Check(!CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  test = evidence;
  test.minimum_architecture = 0x1b0;
  Check(CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));

  for (auto flags : {1u, 2u, 8u, 16u, 6u, 12u, 0xffffffffu}) {
    test = evidence;
    test.flags = flags;
    Check(!CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
  }

  test = evidence;
  test.flags = 0;
  Check(CanRelaxRequirements(test, &mfgunlock::architecture::kAmpere));
}


}  // namespace
int main(int argc, char** argv) {
  try {
    Policy(); StructuralVariants(); InvalidVariants(); RuntimeBounds();
    Composition(Raw(Synthetic())); Composition(Raw(Synthetic()), true);
    Composition(Raw(Synthetic()), false, mfgunlock::architecture::kTuring);
    Composition(Raw(Synthetic()), true, mfgunlock::architecture::kTuring);
    if (argc > 1) {
      Composition(Read(argv[1])); Composition(Read(argv[1]), true);
      for (int index = 2; index < argc; ++index) {
        const auto input = Container(Read(argv[index]));
        const auto* role = temporal::FindTemporalProfile(input.data(), input.size());
        if (role) { Bytes out; std::string why; Check(temporal::BuildTemporalFatbin(input.data(), input.size(), *role, out, why)); }
        else Rejected(input, "external fixture has no resolved local operation");
      }
    }
    std::cout << "assertions=" << g_assertions << '\n'; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
