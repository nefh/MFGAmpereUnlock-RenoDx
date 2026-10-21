// SPDX-License-Identifier: MIT
// Production policy, query forwarding and bounded private-ABI proofs against API
// doubles. An optional local provider file tests real fingerprints read-only;
// the proprietary DLL is never a test fixture distributed with this project.
#define MFG_TEST_STRICT_MODULE_HANDLES
#include "fg_preset.hpp"

#include <climits>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <vector>

static_assert(!std::is_convertible_v<unsigned char*, HMODULE>,
              "module handles must preserve the opaque Windows pointer type");

// A real ABI edge: bool false in AL with nonzero undefined high RAX bits.
// The test body is ours; no NVIDIA instructions are executed.
#if defined(__x86_64__) && !defined(_MSC_VER)
extern "C" bool mfg_test_dirty_bool_false(uint32_t, uint32_t*);
extern "C" bool mfg_test_dirty_bool_true(uint32_t, uint32_t*);
asm(".text\n"
    ".global mfg_test_dirty_bool_false\n"
    "mfg_test_dirty_bool_false:\n"
    "movabs $0x123456789abcde00, %rax\n"
    "ret\n"
    ".global mfg_test_dirty_bool_true\n"
    "mfg_test_dirty_bool_true:\n"
    "movabs $0x123456789abcde01, %rax\n"
    "ret\n");
#endif

namespace preset = mfgunlock::fgpreset;
namespace internal = preset::internal;
namespace {
unsigned g_checks = 0;
unsigned g_native_calls = 0;
unsigned g_report_calls = 0;
uint32_t g_native_value = 2;
bool g_native_result = true;
uint64_t g_structural_result = 1;
void Check(bool condition, const char* reason) {
  ++g_checks;
  if (!condition) throw std::runtime_error(reason);
}
bool NativeRead(uint32_t, uint32_t* value) {
  ++g_native_calls;
  if (value && g_native_result) *value = g_native_value;
  return value && g_native_result;
}
uint64_t StructuralNativeRead(uint32_t, uint32_t* value) {
  ++g_native_calls;
  if (value) *value = g_native_value;
  return g_structural_result;
}
uint32_t NativeReport(void*, void*, const void*) {
  ++g_report_calls;
  return 1;
}
void* Caller(HMODULE module) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(module) + internal::kSelectionReturnRva);
}

void PolicyAndForwarding() {
  for (int value : {INT_MIN, -1, 0, 4, 255, INT_MAX})
    Check(preset::Parse(value) == preset::Preset::kDefault, "invalid/default config preserves passthrough");
  Check(preset::Parse(1) == preset::Preset::kA, "A parser");
  Check(preset::Parse(2) == preset::Preset::kB, "B parser");
  Check(preset::Parse(3) == preset::Preset::kC && std::strcmp(preset::Name(preset::Preset::kC), "C") == 0,
        "C parser and label");
  Check(preset::g_requested.load() == preset::Preset::kDefault, "missing config default");
  Check(preset::g_last_supplied.load() == -1, "no invented supplied value at startup");
  Check(preset::g_last_applied.load() == -1, "no invented applied value at startup");
  auto& entry = internal::g_readers[0].entry;
  entry.identity.module = reinterpret_cast<HMODULE>(0x10000000);
  entry.trampoline = reinterpret_cast<void*>(&NativeRead);
  const auto caller = Caller(entry.identity.module);
  uint32_t value = 77;
  mfgunlock::g_enabled = true;

  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller), "Default forwards success");
  Check(value == 2 && g_native_calls == 1, "Default does not turn driver B into A");
  Check(preset::g_last_supplied == -1, "native answer not mislabeled as override");
  g_native_result = false;
  value = 77;
  Check(!internal::ReadSetting(0, internal::kPresetSetting, &value, caller), "Default preserves failure");
  Check(value == 77, "failed native read preserves caller storage");
  g_native_result = true;

  for (auto choice : {preset::Preset::kA, preset::Preset::kB, preset::Preset::kC}) {
    preset::g_requested = choice;
    const auto before = g_native_calls;
    Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller), "explicit preset read");
    Check(value == static_cast<uint32_t>(choice) && g_native_calls == before, "supplies only selected A/B/C");
    Check(preset::g_last_supplied == static_cast<int>(choice), "records actual supplied value");
  }
  preset::g_requested = preset::Preset::kA;
  Check(preset::g_last_supplied == 3, "editing requested does not fabricate observed A");
  g_native_value = 19;
  Check(internal::ReadSetting(0, 0x104D6667, &value, caller) && value == 19, "frame count setting untouched");
  Check(internal::ReadSetting(0, 0x10E41DF3, &value, caller) && value == 19, "SR preset untouched");
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, nullptr) && value == 19,
        "other callers, including override-state reporting, untouched");
  Check(preset::g_last_supplied == 3, "unrelated queries leave evidence unchanged");
  Check(!internal::ReadSetting(0, internal::kPresetSetting, nullptr, caller), "null output goes to original");
  mfgunlock::g_enabled = false;
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) && value == 19, "addon disabled passthrough");
  mfgunlock::g_enabled = true;
  internal::g_shutting_down = true;
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) && value == 19, "shutdown passthrough");
  internal::g_shutting_down = false;
  preset::g_requested = preset::Preset::kDefault;
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) && value == 19, "return to Default forwards again");
  Check(entry.lock.shared == 0, "reader lock released on every branch");

  internal::g_readers[0].layout.structural = true;
  entry.trampoline = reinterpret_cast<void*>(&StructuralNativeRead);
  for (uint64_t answer : {uint64_t{0}, uint64_t{1}, uint64_t{0x100000001}}) {
    g_structural_result = answer;
    Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) == answer,
          "structural forwarding preserves the complete returned register");
  }
  internal::g_readers[0].layout.cached_bool = true;
  entry.trampoline = reinterpret_cast<void*>(&NativeRead);
  g_native_result = false;
  Check(!internal::ReadSetting(0, internal::kPresetSetting, &value, caller), "cached bool reader preserves native failure");
  g_native_result = true;
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) == 1, "cached bool reader preserves native success");
#if defined(__x86_64__) && !defined(_MSC_VER)
  entry.trampoline = reinterpret_cast<void*>(&mfg_test_dirty_bool_false);
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) == 0,
        "nonzero unused RAX bits must not turn native AL=false into success");
  entry.trampoline = reinterpret_cast<void*>(&mfg_test_dirty_bool_true);
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) == 1,
        "native AL=true is forwarded through its actual bool prototype");
  entry.trampoline = reinterpret_cast<void*>(&NativeRead);
#endif
  preset::g_requested = preset::Preset::kC;
  const auto before_cached = g_native_calls;
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller) == 1 && value == 3 &&
        g_native_calls == before_cached, "cached reader supplies C only to qualified consumer");
  preset::g_requested = preset::Preset::kDefault;
  internal::g_readers[0].layout.cached_bool = false;
  internal::g_readers[0].layout.structural = false;
  entry.trampoline = reinterpret_cast<void*>(&NativeRead);

  auto& observer = internal::g_readers[0].observer;
  observer.trampoline = reinterpret_cast<void*>(&NativeReport);
  alignas(uint32_t) std::array<unsigned char, 0x80> configuration{};
  uint32_t applied = 2;
  std::memcpy(configuration.data() + internal::kAppliedPresetOffset, &applied, sizeof(applied));
  Check(internal::ReportOverrides(0, nullptr, nullptr, configuration.data()) == 1,
        "override-state reporter forwards result");
  Check(g_report_calls == 1 && preset::g_last_applied == 2,
        "provider reporting observes applied B");
  applied = 1;
  std::memcpy(configuration.data() + internal::kAppliedPresetOffset, &applied, sizeof(applied));
  Check(internal::ReportOverrides(0, nullptr, nullptr, configuration.data()) == 1,
        "second override-state report forwards result");
  Check(g_report_calls == 2 && preset::g_last_applied == 1,
        "provider reporting observes applied A");
  applied = 3;
  std::memcpy(configuration.data() + internal::kAppliedPresetOffset, &applied, sizeof(applied));
  Check(internal::ReportOverrides(0, nullptr, nullptr, configuration.data()) == 1 &&
        preset::g_last_applied == 3, "only an actual provider field reports applied C");
  applied = 99;
  std::memcpy(configuration.data() + internal::kAppliedPresetOffset, &applied, sizeof(applied));
  internal::ReportOverrides(0, nullptr, nullptr, configuration.data());
  Check(preset::g_last_applied == 3, "unknown model value does not fabricate an applied preset");
  Check(observer.lock.shared == 0, "observer lock released");

  mfgunlock::hook::UninstallAddress(entry);
  Check(!preset::HasHook(), "no installed hooks after detach");
  Check(!internal::MatchesProvider(nullptr), "missing provider rejected");
  Check(!preset::TryInstall(nullptr), "Default never installs a hook");
}

std::vector<unsigned char> MapProvider(const char* path) {
  std::ifstream file(path, std::ios::binary);
  Check(static_cast<bool>(file), "open optional provider file");
  const std::vector<unsigned char> disk{std::istreambuf_iterator<char>(file), {}};
  auto read32 = [&](size_t offset) {
    Check(offset <= disk.size() && 4 <= disk.size() - offset, "PE field in file");
    uint32_t result;
    std::memcpy(&result, disk.data() + offset, sizeof(result));
    return result;
  };
  const auto nt_offset = read32(0x3C);
  Check(nt_offset <= disk.size() && sizeof(IMAGE_NT_HEADERS64) <= disk.size() - nt_offset, "PE headers in file");
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, disk.data() + nt_offset, sizeof(nt));
  Check(nt.OptionalHeader.SizeOfImage == 0x737000, "test uses the supported provider build");
  std::vector<unsigned char> image(nt.OptionalHeader.SizeOfImage);
  Check(nt.OptionalHeader.SizeOfHeaders <= disk.size() && nt.OptionalHeader.SizeOfHeaders <= image.size(), "header sizes");
  std::memcpy(image.data(), disk.data(), nt.OptionalHeader.SizeOfHeaders);
  const size_t sections = nt_offset + 24 + nt.FileHeader.SizeOfOptionalHeader;
  for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
    const size_t offset = sections + i * sizeof(IMAGE_SECTION_HEADER);
    Check(offset <= disk.size() && sizeof(IMAGE_SECTION_HEADER) <= disk.size() - offset, "section table bounds");
    IMAGE_SECTION_HEADER section{};
    std::memcpy(&section, disk.data() + offset, sizeof(section));
    Check(section.PointerToRawData <= disk.size() && section.SizeOfRawData <= disk.size() - section.PointerToRawData &&
          section.VirtualAddress <= image.size() && section.SizeOfRawData <= image.size() - section.VirtualAddress,
          "section raw and mapped bounds");
    std::memcpy(image.data() + section.VirtualAddress, disk.data() + section.PointerToRawData, section.SizeOfRawData);
  }
  return image;
}

void StructuralDiscovery() {
  using View = mfgunlock::mfggate::View;
  using Range = mfgunlock::mfggate::Range;
  for (const uint32_t shift : {0u, 0x100u, 0x700u}) {
    std::vector<unsigned char> image(0x2000, 0xcc);
    const uint32_t fn = 0x100 + shift, reader = 0x200 + shift;
    std::vector<unsigned char> body = {
        0x48,0x83,0xec,0x28,             // sub rsp,28
        0x48,0x8d,0x54,0x24,0x20,       // lea rdx,[rsp+20]
        0xb9,0xf1,0x1d,0xe4,0x10,       // mov ecx,preset id
        0xe8,0,0,0,0,                  // call settings reader
        0x84,0xc0,                     // test al,al
        0x74,0x10,                     // je failure
        0x8b,0x44,0x24,0x20,           // load native output
        0x83,0xf8,0x02,                 // cmp eax,2
        0x75,0x07,                     // jne other preset
        0xb8,0x02,0,0,0,               // B selection
        0xeb,0x05,                     // common return
        0xb8,0x01,0,0,0,               // A selection/failure
        0x48,0x83,0xc4,0x28,0xc3};
    const int32_t delta = static_cast<int32_t>(reader - (fn + 19));
    std::memcpy(body.data() + 15, &delta, 4);
    std::copy(body.begin(), body.end(), image.begin() + fn);
    const unsigned char read_body[] = {0xc7,0x02,0x02,0,0,0,0xb8,0x01,0,0,0,0xc3};
    std::copy(std::begin(read_body), std::end(read_body), image.begin() + reader);
    std::vector<Range> ranges{{0x100,0x1000,true}};
    View view{image,ranges,{{fn,fn + static_cast<uint32_t>(body.size())},
                           {reader,reader + sizeof(read_body)}}};
    internal::Layout plan;
    Check(internal::ResolveStructuralLayout(view, plan), "relocated unknown preset reader structurally resolved");
    Check(plan.structural && plan.reader_rva == reader && plan.selection_returns.size() == 1 &&
        plan.selection_returns[0] == fn + 19 && plan.observer_rva == 0,
        "reader admission independent of private observer metadata/layout");
    provider_mock::regions.push_back({image.data(), image.size(), MEM_IMAGE, PAGE_EXECUTE_READ});
    const auto module = reinterpret_cast<HMODULE>(image.data());
    Check(internal::LayoutStillMatches(module, plan), "current full reader and consumer witnesses match");
    image[reader + 7] ^= 1;
    Check(!internal::LayoutStillMatches(module, plan), "changed callee return invalidates an old plan before install");
    image[reader + 7] ^= 1;
    image[fn + 29] ^= 1;
    Check(!internal::LayoutStillMatches(module, plan), "changed consumer invalidates an old plan before install");
    image[fn + 29] ^= 1;
    image[0x30] ^= 1;
    Check(internal::LayoutStillMatches(module, plan), "unrelated bytes do not revoke an unchanged operation");
    provider_mock::regions.pop_back();
    image[fn + 10] ^= 1;
    Check(!internal::ResolveStructuralLayout(view, plan), "other setting cannot authorize preset override");
    image[fn + 10] ^= 1;
    image[fn + 26] = 0x24;
    Check(!internal::ResolveStructuralLayout(view, plan), "consumer must read the actual supplied output slot");
    image[fn + 26] = 0x20;
    image[fn + 29] = 3;
    Check(internal::ResolveStructuralLayout(view, plan), "C consumer uses the same proved u32 reader ABI");
    image[fn + 29] = 99;
    Check(!internal::ResolveStructuralLayout(view, plan), "unrelated enum consumer cannot authorize A/B/C setup");
  }
}

bool ReaderProof(const std::vector<unsigned char>& code) {
  std::vector<unsigned char> image(0x1000, 0xcc);
  std::copy(code.begin(), code.end(), image.begin() + 0x100);
  std::vector<mfgunlock::mfggate::Range> ranges{{0x100, 0x400, true}, {0x800, 0x100, false}};
  mfgunlock::mfggate::View view{image, ranges, {{0x100, 0x100 + static_cast<uint32_t>(code.size())}}};
  return internal::ProveReaderAbi(view, 0x100);
}

void ReaderAbiProof() {
  const std::vector<unsigned char> store{0xc7,0x02,0x02,0,0,0};
  const std::vector<unsigned char> yes{0xb8,1,0,0,0,0xc3};
  const auto finish = [&](std::vector<unsigned char> prefix) {
    prefix.insert(prefix.end(), store.begin(), store.end());
    prefix.insert(prefix.end(), yes.begin(), yes.end());
    return prefix;
  };
  Check(ReaderProof(finish({})), "u32 output with full canonical RAX has a provable physical ABI");
  Check(ReaderProof(finish({0x44,0x8b,0xc1})), "R8 may be used after definition from ECX32");
  Check(ReaderProof({0x49,0x89,0xd2,0x41,0xc7,0x02,2,0,0,0,0xb8,1,0,0,0,0xc3}),
        "a full-width copy of RDX retains output pointer provenance");
  Check(ReaderProof({0x48,0x85,0xd2,0x74,0x0c,0xc7,0x02,2,0,0,0,0xb8,1,0,0,0,0xc3,0x31,0xc0,0xc3}),
        "all success/failure paths are analyzed, including null-output failure");
  auto setcc = store;
  const std::vector<unsigned char> flag_return{0x31,0xc0,0x85,0xc9,0x0f,0x95,0xc0,0xc3};
  setcc.insert(setcc.end(), flag_return.begin(), flag_return.end());
  Check(ReaderProof(setcc), "zeroed RAX plus SETcc AL proves a canonical bool return");
  auto movzx = store;
  const std::vector<unsigned char> byte_return{0xb0,1,0x0f,0xb6,0xc0,0xc3};
  movzx.insert(movzx.end(), byte_return.begin(), byte_return.end());
  Check(ReaderProof(movzx), "MOVZX defines the full return after a low-byte bool");
  Check(ReaderProof({0x85,0xc9,0x74,0x08,0xc7,0x02,1,0,0,0,0xeb,0x06,
                    0xc7,0x02,2,0,0,0,0xb8,1,0,0,0,0xc3}),
        "joined success paths each prove the output write");
  std::vector<unsigned char> cached{0x8b,0x05,0,0,0,0,0x89,0x02,0xb8,1,0,0,0,0xc3};
  const int32_t delta = 0x800 - 0x106; std::memcpy(cached.data() + 2, &delta, sizeof(delta));
  Check(ReaderProof(cached), "a bounded native u32 cache read is not another incoming argument");

  for (const auto& [prefix, reason] : std::vector<std::pair<std::vector<unsigned char>, const char*>>{
      {{0x44,0x89,0xc0}, "incoming R8 is an unproved extra argument"},
      {{0x44,0x89,0xc8}, "incoming R9 is an unproved extra argument"},
      {{0x48,0x89,0xc8}, "incoming high RCX bits are outside a u32 argument"},
      {{0x8b,0x44,0x24,0x28}, "entry stack argument reads cannot disappear through forwarding"},
      {{0x66,0x0f,0x7e,0xc0}, "XMM inputs require a different ABI proof"},
      {{0x8b,0xc3}, "incoming nonvolatile register is not a declared argument"},
      {{0xbb,1,0,0,0}, "leaf cannot clobber a nonvolatile register"},
      {{0x48,0x83,0xec,0x08}, "unmodeled stack mutation is not admitted"},
      {{0x65,0x8b,0x02}, "GS prefix cannot be erased from memory provenance"},
      {{0x74,0x00}, "branch cannot use incoming condition flags"},
      {{0xe8,0,0,0,0}, "another native callee needs its own complete ABI proof"}})
    Check(!ReaderProof(finish(prefix)), reason);

  auto low_only = store; low_only.insert(low_only.end(), {0xb0,1,0xc3});
  Check(!ReaderProof(low_only), "AL-only return leaves full RAX unknown for other callers");
  auto wider = store; wider.insert(wider.end(), {0xb8,2,0,0,0,0xc3});
  Check(!ReaderProof(wider), "non-bool EAX result must not be normalized by a bool hook");
  auto upper = store; upper.insert(upper.end(), {0x48,0xb8,1,0,0,0,1,0,0,0,0xc3});
  Check(!ReaderProof(upper), "RAX upper result bits cannot be lost");
  Check(!ReaderProof(yes), "successful return must establish the actual u32 output operation");
  Check(!ReaderProof({0x48,0xc7,0x02,2,0,0,0,0xb8,1,0,0,0,0xc3}), "u64 output exceeds the proven u32 pointee");
  Check(!ReaderProof({0xc7,0x42,4,2,0,0,0,0xb8,1,0,0,0,0xc3}), "extra output fields are not assumed to exist");
  Check(!ReaderProof({0x85,0xc9,0x74,0x06,0xc7,0x02,2,0,0,0,0xb8,1,0,0,0,0xc3}),
        "one success path without an output write invalidates a merged proof");
  auto cleanup = store; cleanup.insert(cleanup.end(), {0xb8,1,0,0,0,0xc2,8,0});
  Check(!ReaderProof(cleanup), "callee-popped stack arguments are not this Windows x64 ABI");
}

void CallerSlotProof() {
  std::vector<unsigned char> original{
      0x48,0x83,0xec,0x28,0x48,0x8d,0x54,0x24,0x20,0xb9,0xf1,0x1d,0xe4,0x10,
      0xe8,0,0,0,0,0x84,0xc0,0x74,0x10,0x8b,0x44,0x24,0x20,0x83,0xf8,0x02,
      0x75,0x07,0xb8,0x02,0,0,0,0xeb,0x05,0xb8,0x01,0,0,0,0x48,0x83,0xc4,0x28,0xc3};
  const auto resolves = [&](std::vector<unsigned char> body) {
    std::vector<unsigned char> image(0x1000, 0xcc);
    const auto call = std::find(body.begin(), body.end(), 0xe8);
    Check(call != body.end(), "fixture contains a direct native call");
    const auto call_offset = static_cast<uint32_t>(call - body.begin());
    const int32_t delta = 0x400 - (0x100 + call_offset + 5);
    std::memcpy(body.data() + call_offset + 1, &delta, sizeof(delta));
    std::copy(body.begin(), body.end(), image.begin() + 0x100);
    const std::vector<unsigned char> reader{0xc7,0x02,2,0,0,0,0xb8,1,0,0,0,0xc3};
    std::copy(reader.begin(), reader.end(), image.begin() + 0x400);
    std::vector<mfgunlock::mfggate::Range> ranges{{0x100, 0x600, true}};
    mfgunlock::mfggate::View view{image, ranges, {{0x100, 0x100 + static_cast<uint32_t>(body.size())},
                                                {0x400, 0x400 + static_cast<uint32_t>(reader.size())}}};
    mfgunlock::mfggate::Graph graph;
    Check(graph.Build(view, view.functions[0]) && !graph.stopped, "mutated caller remains a complete instruction graph");
    internal::Layout layout;
    return internal::ResolveStructuralLayout(view, layout);
  };
  Check(resolves(original), "unchanged native output slot is resolved");
  auto before = original; before.insert(before.begin() + 14, {0x48,0x83,0xec,0x08});
  Check(!resolves(before), "changed RSP between output LEA and call invalidates the slot identity");
  auto after = original; after.insert(after.begin() + 23, {0x48,0x83,0xec,0x08}); after[22] += 4;
  Check(!resolves(after), "changed RSP between call and native consume invalidates the slot identity");
  auto pushed = original; pushed.insert(pushed.begin() + 14, 0x50);
  Check(!resolves(pushed), "implicit PUSH adjustment is also a changed slot base");
  auto bypass = original; bypass.insert(bypass.begin() + 4, {0x85,0xc9,0x74,0x13});
  Check(!resolves(bypass), "another predecessor cannot enter the consumer without the native call");
  auto segmented = original; segmented.insert(segmented.begin() + 23, 0x65); ++segmented[22];
  Check(!resolves(segmented), "segment override is not the same stack output address");
}

void ExactProvider(const char* path) {
  auto image = MapProvider(path);
  auto module = reinterpret_cast<HMODULE>(image.data());
  provider_mock::regions.push_back({module, image.size(), MEM_IMAGE, PAGE_EXECUTE_READ});
  Check(internal::MatchesProvider(module), "real 310.9.1 reader, A/B consumer and reporter fingerprints");
  Check(!internal::MatchesProvider(reinterpret_cast<HMODULE>(reinterpret_cast<uintptr_t>(module) | 1u)),
        "resource-only handle rejected");
  for (auto rva : {internal::kReaderRva, internal::kSelectionRva}) {
    image[rva] ^= 1;
    Check(!internal::MatchesProvider(module), "modified machine code rejected despite matching metadata");
    image[rva] ^= 1;
  }
  image[internal::kReportOverridesRva] ^= 1;
  internal::Layout without_observer;
  Check(internal::MatchesProvider(module) && internal::ResolveLayout(module, without_observer) &&
        without_observer.observer_rva == 0, "changed optional observer cannot veto a proved reader ABI");
  image[internal::kReportOverridesRva] ^= 1;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
  --nt->FileHeader.TimeDateStamp;
  Check(internal::MatchesProvider(module), "timestamp is diagnostic, not admission");
  ++nt->FileHeader.TimeDateStamp;
  provider_mock::regions.back().protect = PAGE_READONLY;
  Check(!internal::MatchesProvider(module), "non-executable reader rejected");
  provider_mock::regions.back().protect = PAGE_EXECUTE_READ | PAGE_GUARD;
  Check(!internal::MatchesProvider(module), "guard page rejected");
  provider_mock::regions.back().protect = PAGE_EXECUTE_READ;
  provider_mock::regions.back().type = MEM_MAPPED;
  Check(!internal::MatchesProvider(module), "non-image mapping rejected");
  provider_mock::regions.back().type = MEM_IMAGE;
  provider_mock::regions.back().bytes = internal::kSelectionRva;
  Check(!internal::MatchesProvider(module), "unreadable consumer rejected");
  provider_mock::regions.back().bytes = image.size();

  Check(!preset::TryInstall(module), "Default avoids even a detour installation");
  preset::g_requested = preset::Preset::kB;
  const auto original_image = image;
  preset_mock::original = reinterpret_cast<void*>(&NativeRead);
  preset_mock::install_ok = false;
  Check(!preset::TryInstall(module), "installation failure is not reported as success");
  const auto retains = provider_mock::retains;
  Check(!preset::TryInstall(module) && provider_mock::retains == retains, "failed retry does not accumulate provider references");
  preset_mock::install_ok = true;
  Check(preset::TryInstall(module), "verified provider hook installation");
  Check(preset::HasHook(), "installed selector hook reported");
  Check(preset::HasObserver(), "installed applied-preset observer reported");
  mfgunlock::hook::UninstallAddress(internal::g_readers[0].observer);
  image[internal::kReaderRva] ^= 1;
  const auto partial_retains = provider_mock::retains;
  preset_mock::fail_install = preset_mock::installs + 1;
  Check(preset::TryInstall(module), "observer retry failure cannot revoke the working reader");
  Check(preset::HasHook() && !preset::HasObserver(), "reader survives observer-only failure");
  image[internal::kReportOverridesRva] ^= 1;
  Check(preset::TryInstall(module) && !preset::HasObserver(), "modified observer stays unhooked without revoking the reader");
  image[internal::kReportOverridesRva] ^= 1;
  preset_mock::fail_install = 0;
  Check(preset::TryInstall(module) && preset::HasObserver(), "retry succeeds with our modified reader prologue");
  Check(provider_mock::retains == partial_retains, "observer retry does not retain the module twice");
  image[internal::kReaderRva] ^= 1;

  const auto installs = preset_mock::installs;
  // Detours changes the prologue in a real installation; a second maintenance
  // pass must recognize its own existing hook before pristine fingerprinting.
  image[internal::kReaderRva] ^= 1;
  Check(preset::TryInstall(module) && preset_mock::installs == installs, "repeated maintenance is idempotent");
  image[internal::kReaderRva] ^= 1;
  Check(image == original_image, "mocked install does not edit any provider setting cache");

  std::array<std::vector<unsigned char>, 4> copies;
  for (size_t i = 0; i < copies.size(); ++i) {
    copies[i] = image;
    provider_mock::regions.push_back({copies[i].data(), copies[i].size(), MEM_IMAGE, PAGE_EXECUTE_READ});
    Check(preset::TryInstall(reinterpret_cast<HMODULE>(copies[i].data())) == (i < 3),
          "bounded independent provider hook slots");
  }
  uint32_t value = 0;
  for (size_t i = 0; i < internal::g_readers.size(); ++i) {
    const auto own_caller = Caller(internal::g_readers[i].retained);
    Check(internal::ReadSetting(i, internal::kPresetSetting, &value, own_caller) && value == 2,
          "each provider uses its own setup call-site and trampoline");
  }
  preset::Shutdown();
  Check(!preset::HasHook(), "all selector hook slots removed on shutdown");
  Check(!preset::HasObserver(), "all observer hook slots removed on shutdown");
  for (auto& reader : internal::g_readers) {
    Check(reader.entry.lock.shared == 0, "no leaked reader lock");
    Check(reader.observer.lock.shared == 0, "no leaked observer lock");
  }
  provider_mock::regions.clear();
}
}  // namespace

int main(int argc, char** argv) {
  PolicyAndForwarding();
  StructuralDiscovery();
  ReaderAbiProof();
  CallerSlotProof();
  if (argc == 2) ExactProvider(argv[1]);
  std::cout << "fg_preset_test: " << g_checks << " checks passed";
  if (argc == 2) std::cout << " (including real provider fingerprints; mocked hooks, no GPU)";
  std::cout << '\n';
}
