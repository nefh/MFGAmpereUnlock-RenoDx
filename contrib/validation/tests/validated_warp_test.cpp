// SPDX-License-Identifier: MIT
// Exercises the production Validated Warp Blend PTX rewrite without embedding
// or redistributing an NVIDIA kernel payload.

#include "../../../src/addons/mfgunlock/validated_warp.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace warp = mfgunlock::validatedwarp::internal;

namespace {

unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (!condition) throw std::runtime_error(reason);
}

std::string Fixture() {
  std::string out = ".version 8.7\n.target sm_120\n.address_size 64\n"
      ".visible .entry Kernel_BlendCandidatesFused(\n"
      ".param .align 8 .b8 Kernel_BlendCandidatesFused_param_0[240]\n) {\n"
      ".reg .pred %p<260>;\n.reg .f32 %f<160>;\n.reg .b32 %r<20>;\n"
      ".reg .b16 %rs<10>;\n.reg .b64 %rd<10>;\n";
  for (auto n : {10, 11}) out += "mov.u32 %r" + std::to_string(n) + ", 1;\n";
  for (auto n : {16, 17}) out += "setp.eq.u32 %p" + std::to_string(n) + ", 0, 0;\n";
  for (auto n : {115,116,117,119,120,121,123,124,125,126,127,129,130,131,132,133,148,149,39,38,37,43,42,41})
    out += "mov.f32 %f" + std::to_string(n) + ", 0f00000000;\n";
  return out + "ld.param.u8 %rs8, [%rd6+220];\nret;\n}\n";
}

template <class T> void Put(std::vector<uint8_t>& out, size_t at, T value) {
  std::memcpy(out.data() + at, &value, sizeof(value));
}
std::vector<uint8_t> PtxContainer(const std::string& text, bool compressed) {
  std::vector<uint8_t> raw(text.begin(), text.end()); raw.push_back(0);
  std::vector<uint8_t> payload;
  if (compressed) {
    payload.push_back(0xf0);
    size_t left = raw.size() - 15;
    while (left >= 255) { payload.push_back(255); left -= 255; }
    payload.push_back(static_cast<uint8_t>(left));
    payload.insert(payload.end(), raw.begin(), raw.end());
  } else payload = raw;
  std::vector<uint8_t> out(80 + ((payload.size() + 7) & ~size_t{7}));
  Put<uint32_t>(out, 0, mfgunlock::fatbin::kMagic); Put<uint16_t>(out, 4, 1);
  Put<uint16_t>(out, 6, 16); Put<uint64_t>(out, 8, out.size() - 16);
  Put<uint16_t>(out, 16, 1); Put<uint16_t>(out, 18, 0x101);
  Put<uint32_t>(out, 20, 64); Put<uint64_t>(out, 24, out.size() - 80);
  Put<uint32_t>(out, 32, compressed ? static_cast<uint32_t>(payload.size()) : 0);
  Put<uint32_t>(out, 44, 120); Put<uint64_t>(out, 56, compressed ? 0x2041 : 0x41);
  Put<uint64_t>(out, 72, compressed ? raw.size() : 0);
  std::copy(payload.begin(), payload.end(), out.begin() + 80);
  return out;
}

void AppendEntry(std::vector<uint8_t>& container, const std::vector<uint8_t>& entry) {
  container.insert(container.end(), entry.begin() + 16, entry.end());
  Put<uint64_t>(container, 8, container.size() - 16);
}

void DescriptorTransactions() {
  namespace mock = provider_mock;
  for (const size_t references : {1u, 3u, 9u, 17u}) {
    std::vector<uint8_t> image(0x5000);
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128;
    Put(image, 0, dos);
    IMAGE_NT_HEADERS64 nt{}; nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64; nt.FileHeader.NumberOfSections = 1;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = image.size(); nt.OptionalHeader.SizeOfHeaders = 1024;
    Put(image, 128, nt);
    IMAGE_SECTION_HEADER section{}; section.VirtualAddress = 0x1000;
    section.Misc.VirtualSize = 0x4000; section.Characteristics = IMAGE_SCN_MEM_READ;
    Put(image, 128 + sizeof(nt), section);
    const auto source = PtxContainer(Fixture(), true);
    std::copy(source.begin(), source.end(), image.begin() + 0x2000);
    const uint64_t original = reinterpret_cast<uint64_t>(image.data() + 0x2000);
    for (size_t i = 0; i < references; ++i) Put(image, 0x1100 + i * 8, original);
    mock::regions.push_back({image.data(), image.size()});
    const auto before = image;
    warp::PreparedRedirect prepared; std::string why;
    Check(warp::PrepareRetargetedRedirect(image.data(), warp::kPtxProfile,
          warp::RewriteValidatedWarpBlend, prepared, why, 86) && prepared.slots.size() == references,
          "actual materialized references are resolved without a fixed count");
    std::vector<mfgunlock::validatedwarp::Redirect> redirects(1);
    Check(warp::CommitPreparedRedirect(prepared, "structural fixture", redirects[0], why),
          "all resolved descriptor references commit");
    for (auto* slot : prepared.slots)
      Check(*slot == reinterpret_cast<uint64_t>(redirects[0].allocation), "each actual descriptor published");
    Check(mfgunlock::validatedwarp::Restore(redirects) && redirects.empty() && before == image,
          "arbitrary descriptor count restores exact source image");
    mock::regions.pop_back();
  }
}

void StructuralSources() {
  std::string why;
  for (const bool compressed : {false, true}) {
    auto text = Fixture() + "// unrelated build change, no fingerprint eligibility\n";
    auto source = PtxContainer(text, compressed);
    size_t at = 0; std::vector<uint8_t> decoded;
    Check(warp::FindPtxEntry(source.data(), source.size(), warp::kPtxProfile, at, decoded, why),
        "actual role found despite arbitrary whole-PTX size/hash and storage encoding");
    Check(std::string(decoded.begin(), decoded.end()) == text, "source decoding exact");
    auto variant = warp::kPtxProfile;
    variant.declared_raw_size = 1; variant.normalized_size = 2; variant.raw_fnv1a64 = 3;
    variant.descriptor_references = 1000;
    Check(warp::FindPtxEntry(source.data(), source.size(), variant, at, decoded, why),
        "historical profile identity/count fields are diagnostic only");
    for (const auto sm : {75u, 86u, 89u, 90u}) {
      std::vector<uint8_t> rebuilt;
      Check(warp::BuildRetargetedFatbin(source.data(), source.size(), variant,
          warp::RewriteValidatedWarpBlend, rebuilt, why, sm), "structural quality target receives attempt");
      std::vector<mfgunlock::fatbin::Entry> entries; size_t end = 0;
      Check(mfgunlock::fatbin::Parse(rebuilt, entries, end) && entries.size() == 1 &&
          entries[0].architecture == sm, "target visible without native-image bypass");
    }
    auto malformed = source;
    Put<uint64_t>(malformed, 24, UINT64_MAX);
    Check(!warp::FindPtxEntry(malformed.data(), malformed.size(), variant, at, decoded, why),
        "bounds failure remains an actual parser failure");
  }
  auto missing = Fixture();
  const std::string definition = "mov.f32 %f123, 0f00000000;\n";
  missing.erase(missing.find(definition), definition.size());
  const auto original = missing;
  Check(!warp::RewriteValidatedWarpBlend(missing, why) && missing == original,
      "missing local input definition cannot result in injected undeclared/unproven data");
  auto changed_register_count = Fixture();
  changed_register_count.replace(changed_register_count.find("%p<260>"), 7, "%p<400>");
  Check(warp::RewriteValidatedWarpBlend(changed_register_count, why), "register count is resolved, not allowlisted");
  auto collision = Fixture(); collision.insert(collision.find("ret;"), ".reg .f32 %qf<20>;\n");
  Check(warp::RewriteValidatedWarpBlend(collision, why) && collision.find("%qf_r6") != collision.npos,
      "fresh registers handle unrelated register-prefix collisions");
  auto spaced = Fixture();
  spaced.replace(spaced.find(".reg .pred %p<260>;"), 19, ".reg\t.pred  %p < 400 > ;");
  Check(warp::RewriteValidatedWarpBlend(spaced, why), "declarations resolved across producer whitespace");
  auto decoy = Fixture() + "// ld.param.u8 %rs8, [%rd6+220];\n// .reg .pred %p<260>;\n";
  Check(warp::RewriteValidatedWarpBlend(decoy, why), "comment anchors do not conflict with real code");
  decoy = Fixture(); const auto at = decoy.find("ld.param.u8"); decoy.insert(at, "// ");
  const auto decoy_before = decoy;
  Check(!warp::RewriteValidatedWarpBlend(decoy, why) && decoy == decoy_before,
      "comment-only insertion point cannot authorize a rewrite");

  auto source = PtxContainer(Fixture(), true);
  auto sibling = PtxContainer(".version 9.9\n.target sm_150\n.address_size 64\n.visible .entry unrelated() {ret;}\n", false);
  Put<uint32_t>(sibling, 44, 150); Put<uint64_t>(sibling, 56, 0x100041);
  AppendEntry(source, sibling);
  auto competing = PtxContainer("native bytes are irrelevant to the chosen PTX", false);
  Put<uint16_t>(competing, 16, 2); Put<uint32_t>(competing, 44, 89);
  AppendEntry(source, competing);
  std::vector<uint8_t> rebuilt;
  Check(warp::BuildRetargetedFatbin(source.data(), source.size(), warp::kPtxProfile,
        warp::RewriteValidatedWarpBlend, rebuilt, why, 89), "quality rewrite with arbitrary native/sibling payload");
  std::vector<mfgunlock::fatbin::Entry> entries; size_t end = 0;
  Check(mfgunlock::fatbin::Parse(rebuilt, entries, end) && entries.size() == 2 &&
        entries[0].architecture == 89 && entries[1].architecture == 150,
        "native competing target cannot bypass requested rewrite");
  Check(std::equal(sibling.begin() + 16, sibling.end(), rebuilt.begin() + entries[1].offset),
        "higher-target sibling header and payload are byte exact");
  std::vector<uint8_t> control;
  Check(warp::BuildRedirectControlFatbin({source.data(), source.size()}, control, why, 89) && control == source,
        "control copy is independent of target whitelist and fixed entry inventory");
}

}  // namespace

int main() {
  using mfgunlock::validatedwarp::ConfiguredMode;
  using mfgunlock::validatedwarp::Mode;
  for (int value : {-1, 0, 1, 2, 3, 255}) {
    const auto expected = value >= 0 && value <= 2 ? static_cast<Mode>(value) : Mode::kValidatedWarp;
    Check(ConfiguredMode(value) == expected,
          "configured Warp mode is independent of UI visibility");
  }
  Check(mfgunlock::validatedwarp::ModeLabel(Mode::kBlackwellBaseline, 89) ==
            "Blackwell baseline sm_89", "Ada diagnostic label");
  Check(mfgunlock::validatedwarp::ModeLabel(Mode::kBlackwellBaseline, 86) ==
            "Blackwell baseline sm_86", "Ampere diagnostic label");
  Check(mfgunlock::validatedwarp::ModeLabel(Mode::kValidatedWarp, 75) ==
            "Validated Warp sm_75", "Turing diagnostic label");
  Check(mfgunlock::validatedwarp::ModeLabel(Mode::kRedirectControl, 75) ==
            "redirect control", "redirect control is target-neutral");

  auto configured = ConfiguredMode(0);
  Check(configured == Mode::kRedirectControl,
        "redirect control can remain selected while advanced UI is hidden");
  configured = ConfiguredMode(static_cast<int>(configured));
  Check(configured == Mode::kRedirectControl,
        "UI visibility never rewrites the selected Warp mode");

  std::string why;
  std::string ptx = Fixture();
  Check(warp::RewriteValidatedWarpBlend(ptx, why), "validated warp rewrite");
  Check(ptx.find("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != std::string::npos,
        "rewrite marker");
  Check(ptx.find(".reg .pred %qv<7>;") != std::string::npos,
        "predicate registers");
  Check(ptx.find(".reg .f32 %qf<12>;") != std::string::npos,
        "float registers");
  Check(ptx.find("ld.param.u8 %rs8, [%rd6+220];") != std::string::npos,
        "original insertion anchor preserved");

  why.clear();
  Check(!warp::RewriteValidatedWarpBlend(ptx, why), "double rewrite rejected");
  Check(why == "validated-warp PTX is already modified", "double rewrite reason");

  why.clear();
  std::string ambiguous = Fixture() + ".reg .pred %p<260>;\n";
  Check(!warp::RewriteValidatedWarpBlend(ambiguous, why), "ambiguous anchor rejected");
  Check(why.find("declaration") != why.npos, "ambiguous declaration reason");

  why.clear();
  std::string missing = Fixture();
  const auto insertion = missing.find("ld.param.u8 %rs8, [%rd6+220];\n");
  missing.erase(insertion, std::string("ld.param.u8 %rs8, [%rd6+220];\n").size());
  Check(!warp::RewriteValidatedWarpBlend(missing, why), "missing anchor rejected");
  Check(why == "required PTX anchor is missing", "missing anchor reason");

  StructuralSources();
  DescriptorTransactions();
  std::cout << "validated_warp_test: " << g_checks << " checks passed\n";
  return 0;
}
