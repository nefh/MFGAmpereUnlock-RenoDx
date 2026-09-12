// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/ptx_retarget.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>

namespace fb = mfgunlock::fatbin;
namespace ptx = mfgunlock::ptx;

using Bytes = std::vector<unsigned char>;

namespace {

unsigned int g_cases = 0;
constexpr std::string_view kPtx =
    ".version 8.7\n"
    ".target sm_89\n"
    ".address_size 64\n"
    ".visible .entry test() { ret; }\n";

void Check(bool value) {
  ++g_cases;
  if (!value) throw std::runtime_error("case " + std::to_string(g_cases));
}

template <class T>
void Put(Bytes& bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

Bytes LiteralBlock(std::string_view text) {
  Bytes output;
  output.push_back(static_cast<unsigned char>(std::min<size_t>(text.size(), 15) << 4));
  if (text.size() >= 15) {
    size_t remaining = text.size() - 15;
    for (; remaining >= 255; remaining -= 255) output.push_back(255);
    output.push_back(static_cast<unsigned char>(remaining));
  }
  output.insert(output.end(), text.begin(), text.end());
  return output;
}

Bytes Container(std::string text, const Bytes* payload = nullptr) {
  if (text.empty() || text.back() != '\0') text.push_back('\0');
  const auto compressed = payload ? *payload : LiteralBlock(text);
  Bytes fatbin(80 + ((compressed.size() + 7) & ~size_t{7}));

  Put<uint32_t>(fatbin, 0, fb::kMagic);
  Put<uint16_t>(fatbin, 4, 1);
  Put<uint16_t>(fatbin, 6, 16);
  Put<uint64_t>(fatbin, 8, fatbin.size() - 16);
  Put<uint16_t>(fatbin, 16, 1);
  Put<uint16_t>(fatbin, 18, 0x101);
  Put<uint32_t>(fatbin, 20, 64);
  Put<uint64_t>(fatbin, 24, fatbin.size() - 80);
  Put<uint32_t>(fatbin, 32, static_cast<uint32_t>(compressed.size()));
  Put<uint32_t>(fatbin, 44, 89);
  Put<uint64_t>(fatbin, 56, 0x2041);
  Put<uint64_t>(fatbin, 72, text.size());
  std::copy(compressed.begin(), compressed.end(), fatbin.begin() + 80);
  return fatbin;
}

void UnitTests() {
  std::string reason;
  ptx::Plan plan;
  const auto valid = Container(std::string(kPtx));

  Check(ptx::Retarget(valid, plan, reason) == ptx::Result::kRetargeted);
  Check(plan.replacement.size() == valid.size());
  Check(fb::ReadU32(plan.replacement.data() + 44) == 86);

  size_t index = 0;
  const auto changed = std::count_if(valid.begin(), valid.end(), [&](unsigned char value) {
    return value != plan.replacement[index++];
  });
  // One PTX literal and one architecture byte change in a single-image fatbin.
  Check(changed == 2);

  // The Ampere profile must emit exactly the same bytes as the original patch.
  auto ampere_expected = valid;
  Put<uint32_t>(ampere_expected, 44, 86);
  const auto literal_header = LiteralBlock(std::string(kPtx) + '\0').size() - kPtx.size() - 1;
  ampere_expected[80 + literal_header + kPtx.find("sm_89") + 4] = '6';
  Check(plan.replacement == ampere_expected);

  Check(ptx::Retarget(plan.replacement, plan, reason) == ptx::Result::kUnchanged);

  for (size_t bytes : {0u, 1u, 15u, 16u, 63u, 79u}) {
    Check(ptx::Retarget(std::span(valid).first(bytes), plan, reason) ==
          ptx::Result::kRejected);
  }
  for (size_t bytes = 80; bytes < valid.size(); ++bytes) {
    Check(ptx::Retarget(std::span(valid).first(bytes), plan, reason) ==
          ptx::Result::kRejected);
  }

  for (const char* tail : {"\n.target sm_89\n", "\n.version 8.8\n", "\n/* open",
                           "\nwgmma.mma_async;\n"}) {
    Check(ptx::Retarget(Container(std::string(kPtx) + tail), plan, reason) ==
          ptx::Result::kRejected);
  }

  Check(ptx::Retarget(Container("/* .target sm_120 */\n" + std::string(kPtx) +
                               "// .target sm_120\n"),
                     plan, reason) == ptx::Result::kRetargeted);
  Check(ptx::Retarget(Container(".file 1 \".target sm_120\"\n" + std::string(kPtx)),
                     plan, reason) == ptx::Result::kRetargeted);

  auto no_nul = valid;
  Put<uint64_t>(no_nul, 72, kPtx.size());
  Check(ptx::Retarget(no_nul, plan, reason) == ptx::Result::kRejected);

  auto bad_arch = valid;
  Put<uint32_t>(bad_arch, 44, 75);
  Check(ptx::Retarget(bad_arch, plan, reason) == ptx::Result::kUnchanged);

  auto bad_flags = valid;
  Put<uint64_t>(bad_flags, 56, 0x2040);
  Check(ptx::Retarget(bad_flags, plan, reason) == ptx::Result::kRejected);

  auto overflow = valid;
  Put<uint64_t>(overflow, 24, UINT64_MAX);
  Check(ptx::Retarget(overflow, plan, reason) == ptx::Result::kRejected);

  Bytes bad_offset{0, 0, 0};
  Bytes bad_offset_output(5);
  Check(!fb::Lz4BlockDecompress(bad_offset.data(), bad_offset.size(),
                                bad_offset_output.data(), bad_offset_output.size()));

  Bytes overlap{0x14, 'a', 1, 0, 0};
  Bytes overlap_output(9);
  Check(fb::Lz4BlockDecompress(overlap.data(), overlap.size(), overlap_output.data(),
                               overlap_output.size()));
  Check(overlap_output == Bytes(9, 'a'));

  // Same literal reused by a match: the target edit must be refused rather than
  // silently changing another decoded byte through the back-reference.
  std::string prefix(kPtx);
  auto block = LiteralBlock(prefix);
  block[0] |= 1;  // match length 5
  const size_t target = prefix.find("sm_89");
  const size_t distance = prefix.size() - target;
  block.push_back(static_cast<unsigned char>(distance));
  block.push_back(static_cast<unsigned char>(distance >> 8));
  block.push_back(0x10);
  block.push_back(0);
  Check(ptx::Retarget(Container(prefix + "sm_89", &block), plan, reason) ==
        ptx::Result::kRejected);
  Check(reason == "target literal is shared with another output byte");

  Check(ptx::Retarget(Container(prefix + "sm_89", &block), plan, reason,
                     mfgunlock::architecture::kTuring) == ptx::Result::kRejected);
  Check(reason == "target literal is shared with another output byte");

  Check(ptx::Retarget(valid, plan, reason, mfgunlock::architecture::kTuring) == ptx::Result::kRetargeted);
  auto turing_expected = valid;
  Put<uint32_t>(turing_expected, 44, 75);
  turing_expected[80 + literal_header + kPtx.find("sm_89") + 3] = '7';
  turing_expected[80 + literal_header + kPtx.find("sm_89") + 4] = '5';
  Check(plan.replacement == turing_expected);
  Check(ptx::Retarget(plan.replacement, plan, reason,
                     mfgunlock::architecture::kTuring) == ptx::Result::kUnchanged);
  Check(ptx::Retarget(valid, plan, reason, mfgunlock::architecture::kAda) == ptx::Result::kUnchanged);
  Check(plan.replacement.empty());

  for (const auto* instruction : {"cp.async.ca.shared.global", "mbarrier.init", "redux.sync.add",
                                  "mma.sp.sync", "cvt.rn.bf16.f32", "mma.sync.aligned.m16n8k16"}) {
    const auto input = Container(std::string(kPtx) + instruction + ";\n");
    Check(ptx::Retarget(input, plan, reason, mfgunlock::architecture::kTuring) == ptx::Result::kRejected);
    Check(ptx::Retarget(input, plan, reason, mfgunlock::architecture::kAmpere) == ptx::Result::kRetargeted);
  }
  Check(ptx::Retarget(Container(std::string(kPtx) + "// cp.async; .tf32; mbarrier.init;\n"),
                     plan, reason, mfgunlock::architecture::kTuring) == ptx::Result::kRetargeted);
  Check(ptx::Retarget(Container(std::string(kPtx) + "mma.sync.aligned.m16n8k8.row.col.f32.f16.f16.f32;\n"),
                     plan, reason, mfgunlock::architecture::kTuring) == ptx::Result::kRetargeted);

  std::mt19937 random(13);
  for (unsigned int i = 0; i < 1000; ++i) {
    auto mutated = valid;
    mutated[random() % mutated.size()] ^=
        static_cast<unsigned char>(1 + random() % 255);
    (void)ptx::Retarget(mutated, plan, reason);  // sanitizer smoke input
    (void)ptx::Retarget(mutated, plan, reason, mfgunlock::architecture::kTuring);
  }
}

void Corpus(const std::filesystem::path& path, const std::filesystem::path& output_path) {
  unsigned int accepted = 0;
  std::filesystem::create_directories(output_path);

  for (const auto& item : std::filesystem::directory_iterator(path)) {
    if (item.path().extension() != ".fatbin") continue;

    std::ifstream input(item.path(), std::ios::binary);
    Bytes bytes{std::istreambuf_iterator<char>(input), {}};
    ptx::Plan plan;
    std::string reason;
    if (ptx::Retarget(bytes, plan, reason) != ptx::Result::kRetargeted) {
      throw std::runtime_error(item.path().string() + ": " + reason);
    }

    std::ofstream output(output_path / item.path().filename(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(plan.replacement.data()),
                 static_cast<std::streamsize>(plan.replacement.size()));
    ++accepted;
  }

  if (accepted == 0) throw std::runtime_error("empty corpus");
  std::cout << "corpus_retargeted=" << accepted << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    UnitTests();
    if (argc == 3) {
      Corpus(argv[1], argv[2]);
    } else if (argc != 1) {
      throw std::runtime_error(
          "usage: ptx_retarget_test [fatbin_directory output_directory]");
    }
    std::cout << "assertions=" << g_cases << ", mutation_smoke_inputs=1000\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
