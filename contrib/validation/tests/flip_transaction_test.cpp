// SPDX-License-Identifier: MIT
// Exact addon writer/restore/selection/cache with simulated mapped Windows memory.
// No DLL is loaded, instruction executed, or GPU operation attempted by this test.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

constexpr DWORD MAX_PATH = 260;
void PatchFrameCountCeiling(HMODULE) {}  // Independent transaction, outside this test.
#include "addon_flip_transaction_seam.hpp"

namespace {
namespace mock = provider_mock;
unsigned checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}

bool journal_precedes_protect = true;
bool protect_within_region = true;
bool module_retained = true;
struct Event { void* address = nullptr; size_t length = 0; DWORD desired = 0; };
std::array<Event, 512> events{};
size_t event_count = 0;

void ObserveProtect(void* address, size_t length, DWORD desired) {
  if (event_count < events.size()) events[event_count++] = {address, length, desired};
  const auto at = reinterpret_cast<uintptr_t>(address);
  const auto found = std::find_if(g_flip_meter_sites.begin(), g_flip_meter_sites.end(),
      [&](const auto& site) {
        const auto base = reinterpret_cast<uintptr_t>(site.address);
        return at >= base && at - base < site.length && length <= site.length - (at - base);
      });
  journal_precedes_protect = journal_precedes_protect && found != g_flip_meter_sites.end();
  MEMORY_BASIC_INFORMATION memory{};
  const bool queried = VirtualQuery(address, &memory, sizeof(memory)) != 0;
  const auto base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
  protect_within_region = protect_within_region && queried && at >= base &&
      at - base < memory.RegionSize && length <= memory.RegionSize - (at - base);
  module_retained = module_retained && mock::retains > mock::releases &&
      std::find(g_flip_meter_modules.begin(), g_flip_meter_modules.end(), memory.AllocationBase) !=
          g_flip_meter_modules.end();
}

void Reset() {
  g_flip_meter_sites.clear();
  g_flip_meter_attempts.clear();
  g_flip_meter_modules.clear();
  g_flip_meter_patched = false;
  g_flip_meter_failed = false;
  g_flip_meter_rollback_pending = false;
  g_flip_meter_scan_lock = {};
  mock::regions.clear();
  mock::paths.clear();
  mock::retain_ok = true;
  mock::retains = mock::releases = 0;
  mock::protection_calls = mock::flush_calls = 0;
  mock::fail_protection_call = mock::fail_flush_call = mock::corrupt_flush_call = 0;
  mock::fail_protection_calls.clear();
  mock::fail_flush_calls.clear();
  mock::corrupt_flush_calls.clear();
  mock::before_protect = ObserveProtect;
  journal_precedes_protect = protect_within_region = module_retained = true;
  event_count = 0;
  reshade::log::lines.clear();
}

// Simulated MEM_IMAGE bases must be page-aligned, like a mapped Windows image.
// Without this, Clang may give the byte-only fixture tagged-handle low bits.
struct alignas(4096) Image {
  std::array<unsigned char, 0x4000> bytes{};
  HMODULE Module() { return bytes.data(); }
  explicit Image(unsigned id = 1, bool immediate = true, bool reg = true) {
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    std::memcpy(bytes.data(), &dos, sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 2;
    nt.FileHeader.TimeDateStamp = id;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfHeaders = 0x400;
    nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(bytes.size());
    std::memcpy(bytes.data() + 0x80, &nt, sizeof(nt));
    IMAGE_SECTION_HEADER text{}, data{};
    text.VirtualAddress = 0x1000;
    text.Misc.VirtualSize = 0x2000;
    text.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    data.VirtualAddress = 0x3000;
    data.Misc.VirtualSize = 0x1000;
    data.Characteristics = IMAGE_SCN_MEM_READ;
    std::memcpy(bytes.data() + 0x80 + sizeof(nt), &text, sizeof(text));
    std::memcpy(bytes.data() + 0x80 + sizeof(nt) + sizeof(text), &data, sizeof(data));
    std::memcpy(bytes.data() + 0x3200, kFlipMarker, sizeof(kFlipMarker));
    const unsigned char lea[] = {0x48, 0x8d, 0x0d};
    std::memcpy(bytes.data() + 0x1000, lea, sizeof(lea));
    const int displacement = 0x3200 - 0x1007;
    std::memcpy(bytes.data() + 0x1003, &displacement, sizeof(displacement));
    const unsigned char fallback[] = {0xc6, 0x80, 0x34, 0x12, 0, 0, 0};
    std::memcpy(bytes.data() + 0x1010, fallback, sizeof(fallback));
    if (immediate) AddImmediate();
    if (reg) {
      const unsigned char store[] = {0x40, 0x88, 0x80, 0x34, 0x12, 0, 0};
      std::memcpy(bytes.data() + 0x1500, store, sizeof(store));
    }
    mock::regions.push_back({Module(), bytes.size(), MEM_IMAGE, PAGE_EXECUTE_READ});
    mock::paths[Module()] = "X:\\synthetic\\unlisted_flip_plugin.dll";
  }
  void AddImmediate() {
    const unsigned char store[] = {0xc6, 0x80, 0x34, 0x12, 0, 0, 1};
    std::memcpy(bytes.data() + 0x1400, store, sizeof(store));
  }
  void Split() {
    std::erase_if(mock::regions, [&](const auto& region) { return region.base == Module(); });
    // The seven-byte register store crosses this protection-region boundary.
    mock::regions.push_back({Module(), 0x1503, MEM_IMAGE, PAGE_EXECUTE_READ, Module()});
    mock::regions.push_back({bytes.data() + 0x1503, bytes.size() - 0x1503,
                            MEM_IMAGE, PAGE_READONLY, Module()});
  }
};

static_assert(alignof(Image) >= 4096);

void CheckApiInvariants() {
  Check(journal_precedes_protect, "undo is registered before every protection change");
  Check(protect_within_region, "no VirtualProtect range crosses an observed protection region");
  Check(module_retained, "every memory mutation has a durable module reference");
}

void SuccessAndCache() {
  Reset();
  Image image;
  const auto original = image.bytes;
  Check(TryPatchFlipMeteringOnce(image.Module(), 1, 1), "actual two-store transaction applies");
  Check(g_flip_meter_sites.size() == 2 && g_flip_meter_patched && !g_flip_meter_failed &&
        !g_flip_meter_rollback_pending, "only complete writes publish readiness");
  Check(g_flip_meter_attempts.size() == 1 &&
        g_flip_meter_attempts[0].result == FlipPatchResult::kApplied, "cache records explicit commit");
  Check(image.bytes[0x1406] == 0 && image.bytes[0x1500] == 0xc6 && image.bytes[0x1506] == 0,
        "the actual production immediate and register-store replacements were written");
  Check(g_flip_meter_modules.size() == 1 && mock::retains == mock::releases + 1,
        "two scan references leave exactly one durable owner");
  const auto writes = mock::protection_calls, flushes = mock::flush_calls, retains = mock::retains;
  for (unsigned i = 0; i < 1000; ++i)
    Check(TryPatchFlipMeteringOnce(image.Module(), 2, i), "committed cache survives settings revision");
  Check(mock::protection_calls == writes && mock::flush_calls == flushes && mock::retains == retains,
        "committed cache neither rewrites nor adds retain references");
  RestoreFlipMetering();
  Check(image.bytes == original && g_flip_meter_sites.empty() && g_flip_meter_attempts.empty() &&
        !g_flip_meter_patched && !g_flip_meter_failed, "successful restore clears journal and cache");
  Check(mock::regions[0].protect == PAGE_EXECUTE_READ && g_flip_meter_modules.size() == 1,
        "original protection restored; process-lifetime reference is not released in teardown");
  CheckApiInvariants();
}

void CleanWriteFailures() {
  for (unsigned failure = 0; failure != 4; ++failure) {
    Reset();
    Image image;
    const auto original = image.bytes;
    if (failure == 0) mock::fail_protection_call = 1;
    if (failure == 1) mock::fail_protection_call = 2;
    if (failure == 2) mock::fail_flush_call = 1;
    if (failure == 3) mock::corrupt_flush_call = 1;
    Check(!TryPatchFlipMeteringOnce(image.Module(), 1, 1), "write failure is not cached as success");
    Check(image.bytes == original && g_flip_meter_sites.empty() && !g_flip_meter_patched &&
          g_flip_meter_failed && !g_flip_meter_rollback_pending, "write failure rolls back exact original bytes");
    Check(mock::regions[0].protect == PAGE_EXECUTE_READ,
          "rollback uses pre-write protection even after failed protection restoration");
    Check(g_flip_meter_attempts[0].result == FlipPatchResult::kNotApplied,
          "complete rollback has no applied or pending cache claim");
    const auto attempts = mock::protection_calls;
    Check(!TryPatchFlipMeteringOnce(image.Module(), 1, 1) && mock::protection_calls == attempts,
          "same completed failed attempt does not continuously rescan/write");
    mock::fail_protection_call = mock::fail_flush_call = mock::corrupt_flush_call = 0;
    Check(TryPatchFlipMeteringOnce(image.Module(), 1, 2), "new user revision rearms a clean failed transaction");
    RestoreFlipMetering();
    Check(image.bytes == original && g_flip_meter_sites.empty(), "retry and later restore remain reversible");
    CheckApiInvariants();
  }
}

void IncompleteRollback() {
  Reset();
  Image image;
  const auto original = image.bytes;
  mock::fail_protection_calls = {2, 3};  // Return-to-RX fails, then rollback cannot open the store.
  Check(!TryPatchFlipMeteringOnce(image.Module(), 1, 1), "incomplete rollback is not an applied result");
  Check(g_flip_meter_sites.size() == 1 && g_flip_meter_sites[0].rollback_pending &&
        g_flip_meter_sites[0].protections[0].original == PAGE_EXECUTE_READ,
        "undo retains exact bytes and original protection after both failures");
  Check(g_flip_meter_rollback_pending && g_flip_meter_failed && !g_flip_meter_patched &&
        g_flip_meter_attempts[0].result == FlipPatchResult::kRollbackIncomplete,
        "pending undo revokes readiness and has an explicit cache state");
  const auto calls = mock::protection_calls;
  Check(!TryPatchFlipMeteringOnce(image.Module(), 50, 60) && mock::protection_calls == calls,
        "context changes cannot append new writes to an unresolved journal");
  Image second(2);
  Check(!TryPatchFlipMeteringOnce(second.Module(), 50, 60) && mock::protection_calls == calls,
        "another plugin cannot hide an unresolved memory transaction");
  mock::fail_protection_calls.clear();
  RestoreFlipMetering();  // g_flip_meter_patched is already false here.
  Check(image.bytes == original && g_flip_meter_sites.empty() && !g_flip_meter_rollback_pending &&
        !g_flip_meter_failed && g_flip_meter_attempts.empty(), "restore retries undo despite readiness=false");
  Check(mock::regions[0].protect == PAGE_EXECUTE_READ, "RWX is never mistaken for original protection");
  Check(TryPatchFlipMeteringOnce(image.Module(), 50, 60), "successful full restore permits the next mutation");
  RestoreFlipMetering();
  CheckApiInvariants();
}

void RestoreFailures() {
  for (unsigned failure = 0; failure != 4; ++failure) {
    Reset();
    Image image(1, true, false);
    const auto original = image.bytes;
    Check(TryPatchFlipMeteringOnce(image.Module(), 1, 1), "restore-failure fixture commits");
    if (failure == 0) mock::fail_protection_call = mock::protection_calls + 1;
    if (failure == 1) mock::fail_protection_call = mock::protection_calls + 2;
    if (failure == 2) mock::fail_flush_call = mock::flush_calls + 1;
    if (failure == 3) mock::corrupt_flush_call = mock::flush_calls + 1;
    RestoreFlipMetering();
    Check(g_flip_meter_sites.size() == 1 && g_flip_meter_rollback_pending &&
          !g_flip_meter_patched && g_flip_meter_failed, "each failed restore phase retains undo");
    Check(g_flip_meter_attempts.size() == 1 &&
          g_flip_meter_attempts[0].result == FlipPatchResult::kRollbackIncomplete,
          "failed restore cannot retain cached APPLIED");
    mock::fail_protection_call = mock::fail_flush_call = mock::corrupt_flush_call = 0;
    RestoreFlipMetering();
    Check(image.bytes == original && mock::regions[0].protect == PAGE_EXECUTE_READ &&
          g_flip_meter_sites.empty() && !g_flip_meter_failed, "retry completes exact restore");
    CheckApiInvariants();
  }
  Reset();
  Image image(1, true, false);
  Check(TryPatchFlipMeteringOnce(image.Module(), 1, 1), "repeated flush-failure fixture commits");
  mock::fail_flush_calls = {2, 3};
  RestoreFlipMetering();
  RestoreFlipMetering();
  Check(g_flip_meter_sites.size() == 1 && g_flip_meter_rollback_pending && image.bytes[0x1406] == 1,
        "original bytes alone do not discharge a pending instruction-cache obligation");
  mock::fail_flush_calls.clear();
  RestoreFlipMetering();
  Check(g_flip_meter_sites.empty() && !g_flip_meter_rollback_pending,
        "successful flush finally discharges the journal");
}

void RegionBoundariesAndBatchIsolation() {
  for (unsigned failure = 0; failure <= 4; ++failure) {
    Reset();
    Image image(1, false, true);
    image.Split();
    const auto original = image.bytes;
    mock::fail_protection_call = failure;
    const bool applied = TryPatchFlipMeteringOnce(image.Module(), 1, 1);
    Check(applied == (failure == 0), "cross-region write reports the actual complete result");
    if (applied) {
      Check(g_flip_meter_sites.size() == 1 && g_flip_meter_sites[0].protection_count == 2 &&
            g_flip_meter_sites[0].protections[0].original == PAGE_EXECUTE_READ &&
            g_flip_meter_sites[0].protections[1].original == PAGE_READONLY,
            "each region stores its own original protection");
      RestoreFlipMetering();
    }
    Check(image.bytes == original && mock::regions[0].protect == PAGE_EXECUTE_READ &&
          mock::regions[1].protect == PAGE_READONLY && g_flip_meter_sites.empty(),
          "partial cross-region preparation or write restores both protections and bytes");
    CheckApiInvariants();
  }
  Reset();
  Image first(1, true, false), second(2);
  const auto first_original = first.bytes, second_original = second.bytes;
  Check(TryPatchFlipMeteringOnce(first.Module(), 1, 1), "first module commits independently");
  mock::fail_protection_call = mock::protection_calls + 3;  // Second module's second store.
  Check(!TryPatchFlipMeteringOnce(second.Module(), 1, 1), "later store failure rejects its module batch");
  Check(second.bytes == second_original && first.bytes != first_original &&
        g_flip_meter_sites.size() == 1 && g_flip_meter_sites[0].module == first.Module() &&
        g_flip_meter_sites[0].committed && g_flip_meter_patched,
        "sites_before isolates rollback from another module's committed undo");
  Check(g_flip_meter_attempts[0].result == FlipPatchResult::kApplied &&
        g_flip_meter_attempts[1].result == FlipPatchResult::kNotApplied,
        "cache distinguishes the two module transaction results");
  mock::fail_protection_call = 0;
  RestoreFlipMetering();
  Check(first.bytes == first_original && second.bytes == second_original, "all original bytes survive isolated rollback");
  CheckApiInvariants();
}

void RetainAndUndoOrder() {
  Reset();
  Image image(1, false, false);
  Check(!TryPatchFlipMeteringOnce(image.Module(), 1, 1) && g_flip_meter_modules.empty() &&
        mock::retains == mock::releases && mock::protection_calls == 0,
        "a no-write scan releases its temporary image references");
  image.AddImmediate();
  mock::retain_ok = false;
  Check(!TryPatchFlipMeteringOnce(image.Module(), 1, 2) && mock::protection_calls == 0,
        "failed retain never leaves executable addresses in the journal");
  mock::retain_ok = true;
  Check(TryPatchFlipMeteringOnce(image.Module(), 1, 2), "retained retry reaches the actual writer");
  RestoreFlipMetering();
  const auto original = image.bytes;
  const unsigned char seven[] = {1, 2, 3, 4, 5, 6, 7}, one = 0xff;
  Check(WriteFlipSite(image.bytes.data() + 0x1400, seven, sizeof(seven)) &&
        WriteFlipSite(image.bytes.data() + 0x1403, &one, 1), "overlapping snapshots exercise strict reverse undo order");
  mock::fail_protection_call = mock::protection_calls + 1;
  RestoreFlipMetering();
  Check(g_flip_meter_sites.size() == 2 && image.bytes[0x1403] == one,
        "failed newest undo does not restore older overlapping snapshots out of order");
  mock::fail_protection_call = 0;
  RestoreFlipMetering();
  Check(image.bytes == original && g_flip_meter_sites.empty(), "reverse retry restores the true pre-transaction bytes");
  CheckApiInvariants();
}
}  // namespace

int main() {
  try {
    SuccessAndCache();
    CleanWriteFailures();
    IncompleteRollback();
    RestoreFailures();
    RegionBoundariesAndBatchIsolation();
    RetainAndUndoOrder();
    std::cout << "PASS: " << checks << " production flip transaction checks (no native/GPU execution)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
