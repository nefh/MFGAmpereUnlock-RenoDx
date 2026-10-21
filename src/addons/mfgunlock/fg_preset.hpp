/*
 * Process-local DLSS-G model selection from actual setting-call data flow.
 * SPDX-License-Identifier: MIT
 *
 * The provider caches driver settings internally. Intercept only its verified
 * model-setup read, not NVAPI/DRS writes or override-state reporting. A later
 * feature creation may reuse that cache, so a game restart can be necessary.
 */
#pragma once

#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <set>

#include "./provider.hpp"
#include "./ngx_hook.hpp"
#include "./fg_preset_cached.hpp"

namespace mfgunlock::fgpreset {

enum class Preset : int { kDefault = 0, kA = 1, kB = 2, kC = 3 };

constexpr Preset Parse(int value) {
  return value == 1 ? Preset::kA : value == 2 ? Preset::kB : value == 3 ? Preset::kC : Preset::kDefault;
}

inline const char* Name(Preset preset) {
  switch (preset) {
    case Preset::kA: return "A";
    case Preset::kB: return "B";
    case Preset::kC: return "C";
    default: return "Application / driver default";
  }
}

inline std::atomic<Preset> g_requested{Preset::kDefault};
// Most recent preset passed to model setup.
inline std::atomic_int g_last_supplied{-1};
// Most recent preset reported by the provider, when available.
inline std::atomic_int g_last_applied{-1};

namespace internal {

// NVIDIA NvApiDriverSettings.h: NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID.
// A=1, B=2, C=3. The call below is a qualified private reader, NOT an NVAPI ABI.
// Resolve and validate the private reader against the loaded provider.
constexpr uint32_t kPresetSetting = 0x10E41DF1u;
constexpr uint32_t kReaderRva = 0x13EF0u;
constexpr uint32_t kSelectionRva = 0x3424Du;
constexpr uint32_t kSelectionReturnRva = 0x34264u;
constexpr uint32_t kReportOverridesRva = 0x37B00u;
constexpr size_t kAppliedPresetOffset = 0x64u;
constexpr size_t kReaderBytes = 0xCDu;
constexpr size_t kSelectionBytes = 0xEDu;
constexpr size_t kReportOverridesBytes = 0x3F7u;
constexpr uint64_t kReaderHash = 0xAAC59E49B79F45FDull;
constexpr uint64_t kSelectionHash = 0x6BB5E4B6D01B4A2Bull;
constexpr uint64_t kReportOverridesHash = 0x45161D995EC231A3ull;

// Verified from pristine nvngx_dlssg.dll 310.9.1.0, SHA256:
// ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82.
// Fingerprints cover the entire reader and the A/B consumer, with no PE base
// relocations in either range. Matching a version string alone is insufficient.
inline uint64_t Fingerprint(const unsigned char* bytes, size_t count) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  return hash;
}

inline bool ReferenceLayoutMatches(HMODULE module, bool own_reader = false) {
  provider::internal::ImageIdentity identity;
  if (!provider::internal::ReadIdentity(module, identity)) return false;
  // Metadata is not a veto. These reader/consumer contexts qualify the known
  // bool prototype independently of the optional applied-preset observer.
  if (identity.image_bytes < kReaderRva + kReaderBytes ||
      identity.image_bytes < kSelectionRva + kSelectionBytes) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(module);
  if (!provider::internal::IsReadable(base + kReaderRva, kReaderBytes, module) ||
      !provider::internal::IsReadable(base + kSelectionRva, kSelectionBytes, module)) return false;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(base + kReaderRva, &memory, sizeof(memory)) ||
      (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
    return false;
  return (own_reader || Fingerprint(base + kReaderRva, kReaderBytes) == kReaderHash) &&
         Fingerprint(base + kSelectionRva, kSelectionBytes) == kSelectionHash;
}

inline bool ReferenceObserverMatches(HMODULE module) {
  provider::internal::ImageIdentity identity;
  if (!provider::internal::ReadIdentity(module, identity) ||
      identity.image_bytes < kReportOverridesRva + kReportOverridesBytes) return false;
  const auto* address = reinterpret_cast<const unsigned char*>(module) + kReportOverridesRva;
  MEMORY_BASIC_INFORMATION memory{};
  return provider::internal::IsReadable(address, kReportOverridesBytes, module) &&
      VirtualQuery(address, &memory, sizeof(memory)) &&
      (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
      Fingerprint(address, kReportOverridesBytes) == kReportOverridesHash;
}

struct Layout {
  uint32_t reader_rva = kReaderRva;
  std::vector<uint32_t> selection_returns{kSelectionReturnRva};
  uint32_t observer_rva = kReportOverridesRva;
  size_t applied_offset = kAppliedPresetOffset;
  bool structural = false;
  bool cached_bool = false;
  std::vector<mfggate::Witness> witnesses;
};

inline bool LayoutStillMatches(HMODULE module, const Layout& layout) {
  if (layout.witnesses.empty()) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(module);
  for (const auto& witness : layout.witnesses) {
    if (witness.bytes.empty() || !provider::internal::IsReadable(base + witness.rva, witness.bytes.size(), module) ||
        std::memcmp(base + witness.rva, witness.bytes.data(), witness.bytes.size()) != 0) return false;
  }
  return true;
}

inline const mfggate::Instruction* ArgumentDefinition(
    const mfggate::Graph& graph, uint32_t before, unsigned reg) {
  for (unsigned n = 0; n < 32; ++n) {
    const auto* previous = graph.Previous(before);
    const auto preds = graph.predecessors.find(before);
    if (!previous || previous->call || previous->jump || previous->terminal ||
        preds == graph.predecessors.end() || preds->second.size() != 1 ||
        preds->second.front() != previous->rva) return nullptr;
    if (mfggate::WritesRegister(*previous, reg)) return previous;
    before = previous->rva;
  }
  return nullptr;
}

// This whole-target detour must preserve every caller's physical argument and
// return ABI. A caller using ECX/RDX and testing AL is not enough to prove that.
// The structural path therefore interprets the complete reachable leaf reader:
// only RCX32 and RDX enter defined; no incoming stack/XMM/other register can be
// consumed. Successful returns write exactly a u32 through the original output
// pointer, and every return defines full RAX as 0 or 1. Calls/tail calls and
// unmodeled instructions remain an unresolved ABI, not a DLL-version verdict.
enum class ReaderValueKind : uint8_t { kUnknown, kWord, kOutput, kConstant, kBoolean };
struct ReaderValue {
  ReaderValueKind kind = ReaderValueKind::kUnknown;
  unsigned bits = 0;
  uint64_t constant = 0;
  bool operator==(const ReaderValue&) const = default;
};
struct ReaderState {
  std::array<ReaderValue, 16> values{};
  bool flags = false;
  bool output_written = false;
  bool operator==(const ReaderState&) const = default;
};
inline bool ReaderBoolean(const ReaderValue& value) {
  return value.kind == ReaderValueKind::kBoolean ||
      (value.kind == ReaderValueKind::kConstant && value.constant <= 1);
}
inline ReaderValue MergeReaderValue(const ReaderValue& left, const ReaderValue& right) {
  if (left == right) return left;
  const unsigned bits = (std::min)(left.bits, right.bits);
  if (!bits) return {};
  if (left.kind == ReaderValueKind::kOutput && right.kind == ReaderValueKind::kOutput)
    return {ReaderValueKind::kOutput, bits, 0};
  if (ReaderBoolean(left) && ReaderBoolean(right)) return {ReaderValueKind::kBoolean, bits, 0};
  return {ReaderValueKind::kWord, bits, 0};
}
inline bool MergeReaderState(ReaderState& into, const ReaderState& other) {
  auto merged = into;
  for (size_t i = 0; i < into.values.size(); ++i)
    merged.values[i] = MergeReaderValue(into.values[i], other.values[i]);
  merged.flags = into.flags && other.flags;
  merged.output_written = into.output_written && other.output_written;
  if (merged == into) return false;
  into = merged; return true;
}
inline bool ReaderVolatile(unsigned reg) {
  return reg == 0 || reg == 1 || reg == 2 || (reg >= 8 && reg <= 11);
}
inline bool ReaderPrefixes(const mfggate::View& view, const mfggate::Instruction& instruction) {
  size_t at = instruction.rva;
  if (view.image[at] >= 0x40 && view.image[at] <= 0x4f) ++at;
  // In particular, FS/GS changes effective addresses and may carry an implicit
  // input. Do not let the generic decoder's masked prefix erase that meaning.
  return at < instruction.next && view.image[at] == instruction.op;
}
inline bool ProveReaderAbi(const mfggate::View& view, uint32_t target) {
  const auto function = view.Function(target);
  if (function.first != target) return false;
  mfggate::Graph graph;
  if (!graph.Build(view, function) || graph.stopped || graph.instructions.empty()) return false;
  ReaderState entry;
  entry.values[1] = {ReaderValueKind::kWord, 32, 0};
  entry.values[2] = {ReaderValueKind::kOutput, 64, 0};
  std::map<uint32_t, ReaderState> states{{target, entry}};
  std::vector<uint32_t> pending{target};
  bool returned = false, successful = false;
  size_t budget = graph.instructions.size() * 128 + 1;
  while (!pending.empty()) {
    if (!budget--) return false;
    const uint32_t at = pending.back(); pending.pop_back();
    const auto* instruction = graph.At(at);
    if (!instruction || !ReaderPrefixes(view, *instruction)) return false;
    const auto& i = *instruction;
    ReaderState state = states.at(at);
    const auto read = [&](unsigned reg, unsigned bits) {
      return reg < state.values.size() && state.values[reg].bits >= bits;
    };
    const auto write = [&](unsigned reg, ReaderValue value, unsigned width) {
      if (!ReaderVolatile(reg) || (width != 32 && width != 64)) return false;
      if (width == 32) {
        if (value.kind == ReaderValueKind::kOutput) value.kind = ReaderValueKind::kWord;
        if (value.kind == ReaderValueKind::kConstant) value.constant &= UINT32_MAX;
      }
      value.bits = 64; state.values[reg] = value; return true;
    };
    const auto output_memory = [&]() {
      return i.memory && !i.rip && i.index == 16 && i.displacement == 0 &&
          read(i.rm, 64) && state.values[i.rm].kind == ReaderValueKind::kOutput;
    };
    if (i.call) return false;
    if (i.terminal) {
      const auto& result = state.values[0];
      if (i.op != 0xc3 || result.bits != 64 || !ReaderBoolean(result)) return false;
      const bool can_succeed = result.kind == ReaderValueKind::kBoolean || result.constant == 1;
      if (can_succeed && !state.output_written) return false;
      returned = true; successful |= can_succeed; continue;
    }
    if (i.jump) {
      if (i.conditional && !state.flags) return false;
    } else if ((i.op == 0x90 && !(i.rex & 1)) ||
               (i.op == 0x0f && i.op2 == 0x1f && (i.reg & 7) == 0)) {
      // Architectural NOP has no register or memory dependency.
    } else if (i.op >= 0xb8 && i.op <= 0xbf) {
      if (!write(i.reg, {ReaderValueKind::kConstant, 64, i.immediate}, i.width)) return false;
    } else if (i.op == 0xb0 && !(i.rex & 1) && i.immediate <= 1) {
      const bool upper_zero = state.values[0].kind == ReaderValueKind::kConstant &&
          state.values[0].bits == 64 && state.values[0].constant <= 255;
      state.values[0] = {ReaderValueKind::kConstant, upper_zero ? 64u : 8u, i.immediate};
    } else if ((i.op == 0x89 || i.op == 0x8b) && !i.memory) {
      const unsigned source = i.op == 0x89 ? i.reg : i.rm;
      const unsigned destination = i.op == 0x89 ? i.rm : i.reg;
      if (!read(source, i.width) || !write(destination, state.values[source], i.width)) return false;
    } else if ((i.op == 0x89 || (i.op == 0xc7 && (i.reg & 7) == 0)) && i.memory) {
      if (i.width != 32 || !output_memory() || (i.op == 0x89 && !read(i.reg, 32))) return false;
      state.output_written = true;
    } else if (i.op == 0x8b && i.memory) {
      if (i.width != 32 || !(output_memory() ||
          (i.rip && view.InSection(i.target, 4, false))) ||
          !write(i.reg, {ReaderValueKind::kWord, 64, 0}, 32)) return false;
    } else if ((i.op == 0x31 || i.op == 0x33) && !i.memory) {
      const unsigned destination = i.op == 0x31 ? i.rm : i.reg;
      const unsigned source = i.op == 0x31 ? i.reg : i.rm;
      ReaderValue value{ReaderValueKind::kWord, 64, 0};
      if (source == destination) value = {ReaderValueKind::kConstant, 64, 0};
      else if (!read(source, i.width) || !read(destination, i.width)) return false;
      if (!write(destination, value, i.width)) return false;
      state.flags = true;
    } else if ((i.op == 0x85 || i.op == 0x84 || i.op == 0x39 || i.op == 0x3b) && !i.memory) {
      const unsigned width = i.op == 0x84 ? 8 : i.width;
      // High-byte register encodings need a separate lane proof.
      if ((width == 8 && !i.rex && (i.reg >= 4 || i.rm >= 4)) ||
          !read(i.reg, width) || !read(i.rm, width)) return false;
      state.flags = true;
    } else if (!i.memory && (i.op == 0x3d || ((i.op == 0x81 || i.op == 0x83) && (i.reg & 7) == 7))) {
      if (!read(i.rm, i.width)) return false;
      state.flags = true;
    } else if (i.op == 0x0f && i.op2 >= 0x90 && i.op2 <= 0x9f && !i.memory && i.rm == 0 &&
               (i.reg & 7) == 0 && state.flags) {
      const bool upper_zero = state.values[0].kind == ReaderValueKind::kConstant &&
          state.values[0].bits == 64 && state.values[0].constant <= 255;
      state.values[0] = {ReaderValueKind::kBoolean, upper_zero ? 64u : 8u, 0};
    } else if (i.op == 0x0f && i.op2 == 0xb6 && !i.memory && i.rm == 0 && read(0, 8)) {
      auto value = state.values[0];
      if (value.kind == ReaderValueKind::kConstant) value.constant &= 255;
      else if (!ReaderBoolean(value)) value.kind = ReaderValueKind::kWord;
      if (!write(i.reg, value, i.width)) return false;
    } else return false;

    const auto enqueue = [&](uint32_t next) {
      if (!graph.At(next)) return false;
      const auto [it, inserted] = states.emplace(next, state);
      if (inserted || MergeReaderState(it->second, state)) pending.push_back(next);
      return true;
    };
    if (i.jump && !enqueue(i.target)) return false;
    if ((!i.jump || i.conditional) && !enqueue(i.next)) return false;
  }
  return returned && successful;
}
inline bool ChangesSlotBase(const mfggate::Instruction& instruction, unsigned base) {
  return mfggate::WritesRegister(instruction, base) ||
      (base == 4 && ((instruction.op >= 0x50 && instruction.op <= 0x5f) ||
                    instruction.op == 0x68 || instruction.op == 0x6a));
}
inline bool StableSlotUntilCall(const mfggate::View& view, const mfggate::Graph& graph, const mfggate::Instruction* output,
                               const mfggate::Instruction* call) {
  if (!ReaderPrefixes(view, *output)) return false;
  const auto* previous = output;
  for (unsigned step = 0; previous && step < 32; ++step) {
    const auto* cursor = graph.Next(previous);
    if (!mfggate::LinearEdge(graph, previous, cursor)) return false;
    if (cursor == call) return true;
    if (!ReaderPrefixes(view, *cursor) || cursor->call || cursor->terminal || cursor->jump ||
        ChangesSlotBase(*cursor, output->rm)) return false;
    previous = cursor;
  }
  return false;
}

// A private bool ReadSetting(u32, u32*) call is located by actual arguments
// and its A/B consumer. No address, DLL version or code hash is required for
// this route. The optional reporting observer has a separate ABI witness and
// its absence never stops a successfully resolved setting-reader attempt.
inline bool ResolveStructuralLayout(const mfggate::View& view, Layout& layout) {
  layout = {}; layout.reader_rva = 0; layout.selection_returns.clear();
  layout.observer_rva = 0; layout.applied_offset = 0; layout.structural = true;
  std::set<uint32_t> readers;
  std::map<uint32_t, uint32_t> witness_ranges;
  for (const auto& function : view.functions) {
    if (function.second - function.first > mfggate::kMaxFunctionBytes) continue;
    bool mentions = false;
    for (size_t at = function.first; at + 4 <= function.second; ++at) {
      if (view.U32(at) == kPresetSetting) { mentions = true; break; }
    }
    if (!mentions) continue;
    mfggate::Graph graph;
    if (!graph.Build(view, function) || graph.stopped) continue;
    for (const auto& [at, call] : graph.instructions) {
      if (!call.call || call.op != 0xe8 || view.Function(call.target).first != call.target) continue;
      const auto* id = ArgumentDefinition(graph, at, 1);
      const auto* output = ArgumentDefinition(graph, at, 2);
      unsigned argument = 0; uint32_t setting = 0;
      if (!mfggate::IsImmMov(id, argument, setting) || argument != 1 || setting != kPresetSetting ||
          !output || output->op != 0x8d || output->width != 64 || !output->memory ||
          output->reg != 2 || output->rip || output->index != 16 ||
          (output->rm != 4 && output->rm != 5) || !StableSlotUntilCall(view, graph, output, &call)) continue;
      const auto* test = graph.Next(&call);
      if (!test || !ReaderPrefixes(view, *test) || (test->op != 0x84 && test->op != 0x85) || test->memory ||
          test->reg != 0 || test->rm != 0 || !mfggate::LinearEdge(graph, &call, test)) continue;
      const auto* branch = graph.Next(test);
      if (!branch || !branch->conditional || !mfggate::LinearEdge(graph, test, branch)) continue;
      // Native success (AL != 0) must flow to a 32-bit load of exactly the
      // output slot, then a literal A/B/C selection comparison on that value.
      const bool zero = branch->op == 0x74 || (branch->op == 0x0f && branch->op2 == 0x84);
      const bool nonzero = branch->op == 0x75 || (branch->op == 0x0f && branch->op2 == 0x85);
      if (!zero && !nonzero) continue;
      const auto* cursor = graph.At(zero ? branch->next : branch->target);
      const auto incoming = cursor ? graph.predecessors.find(cursor->rva) : graph.predecessors.end();
      if (incoming == graph.predecessors.end() || incoming->second.size() != 1 ||
          incoming->second.front() != branch->rva) continue;
      unsigned value_register = 16;
      bool consumed = false;
      for (unsigned step = 0; cursor && step < 24; ++step) {
        if (!ReaderPrefixes(view, *cursor) || cursor->call || cursor->terminal ||
            ChangesSlotBase(*cursor, output->rm)) break;
        if (cursor->op == 0x8b && cursor->width == 32 && cursor->memory && !cursor->rip &&
            cursor->rm == output->rm && cursor->index == output->index &&
            cursor->displacement == output->displacement) value_register = cursor->reg;
        else if (value_register < 16 && !cursor->memory && cursor->width == 32 &&
            (cursor->op == 0x3d || ((cursor->op == 0x81 || cursor->op == 0x83) && (cursor->reg & 7) == 7)) &&
            cursor->rm == value_register && (cursor->immediate == 1 || cursor->immediate == 2 ||
                                               cursor->immediate == 3)) {
          const auto* selection = graph.Next(cursor);
          consumed = selection && selection->conditional && mfggate::LinearEdge(graph, cursor, selection);
          break;
        } else if (value_register < 16 && mfggate::WritesRegister(*cursor, value_register)) break;
        if (cursor->jump) break;
        const auto* next = graph.Next(cursor);
        if (!mfggate::LinearEdge(graph, cursor, next)) break;
        cursor = next;
      }
      if (!consumed) continue;
      cached::Evidence cache_evidence;
      const bool word_abi = ProveReaderAbi(view, call.target);
      const bool bool_abi = !word_abi && cached::Qualify(view, call.target, cache_evidence);
      if ((!word_abi && !bool_abi) || (bool_abi && test->op != 0x84)) continue;
      if (bool_abi) {
        // The verified native reader defines AL only. It must NOT be called
        // as a uint64_t-returning function and tested using undefined high bits.
        layout.cached_bool = true;
        for (const auto& range : cache_evidence.functions) witness_ranges.emplace(range.first, range.second);
      }
      readers.insert(call.target); layout.selection_returns.push_back(call.next);
      witness_ranges.emplace(function.first, function.second);
      const auto reader = view.Function(call.target);
      witness_ranges.emplace(reader.first, reader.second);
    }
  }
  if (readers.size() != 1 || layout.selection_returns.empty()) {
    layout.selection_returns.clear(); return false;
  }
  layout.reader_rva = *readers.begin();
  std::sort(layout.selection_returns.begin(), layout.selection_returns.end());
  layout.selection_returns.erase(std::unique(layout.selection_returns.begin(),
      layout.selection_returns.end()), layout.selection_returns.end());
  for (const auto& [begin, end] : witness_ranges)
    layout.witnesses.push_back({begin, {view.image.begin() + begin, view.image.begin() + end}});
  return true;
}

inline bool ResolveLayout(HMODULE module, Layout& layout) {
  if (ReferenceLayoutMatches(module)) {
    layout = {};
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    layout.witnesses.push_back({kReaderRva, {base + kReaderRva, base + kReaderRva + kReaderBytes}});
    layout.witnesses.push_back({kSelectionRva, {base + kSelectionRva, base + kSelectionRva + kSelectionBytes}});
    if (!ReferenceObserverMatches(module)) { layout.observer_rva = 0; layout.applied_offset = 0; }
    return true;
  }
  provider::internal::Image image;
  if (!provider::internal::IsImageMapping(module) || !provider::internal::InspectImage(module, image)) return false;
  std::vector<mfggate::Range> ranges;
  for (const auto& section : image.sections) {
    if (!section.Misc.VirtualSize || !provider::internal::IsReadable(image.base + section.VirtualAddress,
        section.Misc.VirtualSize, module)) continue;
    ranges.push_back({section.VirtualAddress, section.Misc.VirtualSize,
        (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
  }
  mfggate::View view{{image.base, image.bytes}, ranges, {}};
  return view.ReadFunctions() && ResolveStructuralLayout(view, layout);
}

inline bool MatchesProvider(HMODULE module, bool own_reader = false) {
  if (own_reader) return ReferenceLayoutMatches(module, true);
  Layout layout;
  return ResolveLayout(module, layout);
}

// A full register return preserves the leaf proof for AL/EAX/RAX callers.
// The reference and qualified cached readers have a bool native prototype.
// Those are forwarded via ReferenceReadSettingFn and normalized to 0/1.
using ReadSettingFn = uint64_t (__cdecl*)(uint32_t setting, uint32_t* value);
using ReferenceReadSettingFn = bool (__cdecl*)(uint32_t setting, uint32_t* value);
using ReportOverridesFn = uint32_t (__cdecl*)(void* core, void* states, const void* configuration);
struct ReaderHook {
  hook::AddressHook entry;
  hook::AddressHook observer;
  HMODULE retained = nullptr;
  Layout layout{};
};
// Like the NGX bridge, keep a separate trampoline for each loaded provider.
inline std::array<ReaderHook, 4> g_readers;
inline std::atomic_bool g_shutting_down{false};

inline uint64_t ReadSetting(size_t index, uint32_t setting, uint32_t* value, const void* caller) {
  auto& entry = g_readers[index].entry;
  // Keep teardown from freeing the trampoline during a forwarded call.
  AcquireSRWLockShared(&entry.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockShared(lock); }
  } unlock{&entry.lock};
  const auto real = entry.Original<ReadSettingFn>();
  if (!real) return false;

  const auto preset = g_requested.load(std::memory_order_relaxed);
  const auto base = reinterpret_cast<uintptr_t>(entry.identity.module);
  const auto return_address = reinterpret_cast<uintptr_t>(caller);
  const bool selection_return = return_address >= base &&
      std::find(g_readers[index].layout.selection_returns.begin(),
                g_readers[index].layout.selection_returns.end(), return_address - base) !=
          g_readers[index].layout.selection_returns.end();
  if (g_enabled.load(std::memory_order_relaxed) && !g_shutting_down.load(std::memory_order_relaxed) &&
      value && setting == kPresetSetting && preset != Preset::kDefault &&
      selection_return) {
    *value = static_cast<uint32_t>(preset);
    if (g_last_supplied.exchange(static_cast<int>(preset), std::memory_order_relaxed) != static_cast<int>(preset))
      provider::Log(std::string("FG model-setup query supplied Preset ") + Name(preset) +
                    "");
    return true;
  }
  // Includes Default, all other settings, and the provider's reporting reads.
  if (g_readers[index].layout.structural && !g_readers[index].layout.cached_bool) return real(setting, value);
  return entry.Original<ReferenceReadSettingFn>()(setting, value) ? uint64_t{1} : uint64_t{0};
}

template <size_t Index>
inline uint64_t __cdecl HookedReadSetting(uint32_t setting, uint32_t* value) {
#ifdef _MSC_VER
  const void* caller = _ReturnAddress();
#else
  const void* caller = __builtin_return_address(0);
#endif
  return ReadSetting(Index, setting, value, caller);
}

inline constexpr std::array<ReadSettingFn, 4> kReaders = {
    HookedReadSetting<0>, HookedReadSetting<1>, HookedReadSetting<2>, HookedReadSetting<3>};

inline uint32_t ReportOverrides(size_t index, void* core, void* states,
                                const void* configuration) {
  auto& observer = g_readers[index].observer;
  AcquireSRWLockShared(&observer.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockShared(lock); }
  } unlock{&observer.lock};
  const auto real = observer.Original<ReportOverridesFn>();
  if (!real) return 0xBAD00000u;

  uint32_t applied = 0;
  if (configuration) {
    const auto* bytes = static_cast<const unsigned char*>(configuration);
    std::memcpy(&applied, bytes + g_readers[index].layout.applied_offset, sizeof(applied));
  }
  const uint32_t result = real(core, states, configuration);
  if (g_last_supplied.load(std::memory_order_relaxed) >= 0 &&
      (applied == static_cast<uint32_t>(Preset::kA) ||
       applied == static_cast<uint32_t>(Preset::kB) ||
       applied == static_cast<uint32_t>(Preset::kC))) {
    const int value = static_cast<int>(applied);
    if (g_last_applied.exchange(value, std::memory_order_relaxed) != value)
      provider::Log(std::string("FG provider reports applied render preset ") +
                    Name(Parse(value)) + " (runtime-observed)");
  }
  return result;
}

template <size_t Index>
inline uint32_t __cdecl HookedReportOverrides(void* core, void* states,
                                              const void* configuration) {
  return ReportOverrides(Index, core, states, configuration);
}

inline constexpr std::array<ReportOverridesFn, 4> kObservers = {
    HookedReportOverrides<0>, HookedReportOverrides<1>,
    HookedReportOverrides<2>, HookedReportOverrides<3>};

}  // namespace internal

// Called only by the existing serialized provider-maintenance path. Do not
// install from Present or from inside the setting-reader callback.
inline bool TryInstall(HMODULE module) {
  if (!g_enabled.load() || internal::g_shutting_down.load() ||
      g_requested.load() == Preset::kDefault) return false;
  for (size_t i = 0; i < internal::g_readers.size(); ++i) {
    auto& reader = internal::g_readers[i];
    if (reader.retained != module || !reader.entry.installed.load(std::memory_order_acquire)) continue;
    if (!reader.layout.observer_rva || reader.observer.installed.load(std::memory_order_acquire)) return true;
    const auto target = reinterpret_cast<unsigned char*>(module) + reader.layout.reader_rva;
    if (reader.entry.target.load(std::memory_order_acquire) != target ||
        !internal::ReferenceLayoutMatches(module, true) || !internal::ReferenceObserverMatches(module)) return true;
    // Observing applied state is optional and cannot revoke an installed
    // reader or suppress the actual requested model-setting trial.
    hook::InstallAddress(reader.observer,
        reinterpret_cast<unsigned char*>(module) + reader.layout.observer_rva,
        reinterpret_cast<void*>(internal::kObservers[i]), "DLSS-G render preset observer");
    return true;
  }
  internal::Layout layout;
  if (!internal::ResolveLayout(module, layout)) return false;
  for (size_t i = 0; i < internal::g_readers.size(); ++i) {
    auto& reader = internal::g_readers[i];
    if (reader.retained && reader.retained != module) continue;
    if (!reader.retained) {
      HMODULE held = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module), &held) || held != module) return false;
      reader.retained = held;
    }
    // Compare the complete current ABI/consumer evidence after retaining this
    // exact mapping and immediately before the hook transaction.
    if (!internal::LayoutStillMatches(module, layout)) return false;
    reader.layout = layout;
    if (!hook::InstallAddress(reader.entry,
          reinterpret_cast<unsigned char*>(module) + layout.reader_rva,
          reinterpret_cast<void*>(internal::kReaders[i]), "DLSS-G model preset reader")) return false;
    const bool observed = layout.observer_rva && internal::ReferenceObserverMatches(module) && hook::InstallAddress(reader.observer,
        reinterpret_cast<unsigned char*>(module) + layout.observer_rva,
        reinterpret_cast<void*>(internal::kObservers[i]), "DLSS-G render preset observer");
    provider::Log(observed
        ? "FG preset reader and independently resolved applied-preset observer installed"
        : "FG preset reader installed");
    return true;
  }
  return false;
}

inline bool HasHook() {
  for (auto& reader : internal::g_readers) {
    if (reader.entry.installed.load(std::memory_order_acquire)) return true;
  }
  return false;
}

inline bool HasObserver() {
  for (auto& reader : internal::g_readers) {
    if (reader.observer.installed.load(std::memory_order_acquire)) return true;
  }
  return false;
}

inline void Shutdown() {
  internal::g_shutting_down.store(true, std::memory_order_release);
  for (auto& reader : internal::g_readers) {
    hook::UninstallAddress(reader.observer);
    hook::UninstallAddress(reader.entry);
  }
}

}  // namespace mfgunlock::fgpreset
