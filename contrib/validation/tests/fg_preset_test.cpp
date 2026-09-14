// SPDX-License-Identifier: MIT
// Production policy, query forwarding and exact-build guards against API
// doubles. An optional local provider file tests real fingerprints read-only;
// the proprietary DLL is never a test fixture distributed with this project.
#include "fg_preset.hpp"

#include <climits>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace preset = mfgunlock::fgpreset;
namespace internal = preset::internal;
namespace {
unsigned g_checks = 0;
unsigned g_native_calls = 0;
unsigned g_report_calls = 0;
uint32_t g_native_value = 2;
bool g_native_result = true;
void Check(bool condition, const char* reason) {
  ++g_checks;
  if (!condition) throw std::runtime_error(reason);
}
bool NativeRead(uint32_t, uint32_t* value) {
  ++g_native_calls;
  if (value && g_native_result) *value = g_native_value;
  return value && g_native_result;
}
uint32_t NativeReport(void*, void*, const void*) {
  ++g_report_calls;
  return 1;
}
void* Caller(HMODULE module) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(module) + internal::kSelectionReturnRva);
}

void PolicyAndForwarding() {
  for (int value : {INT_MIN, -1, 0, 3, 255, INT_MAX})
    Check(preset::Parse(value) == preset::Preset::kDefault, "invalid/default config preserves passthrough");
  Check(preset::Parse(1) == preset::Preset::kA, "A parser");
  Check(preset::Parse(2) == preset::Preset::kB, "B parser");
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

  for (auto choice : {preset::Preset::kA, preset::Preset::kB}) {
    preset::g_requested = choice;
    const auto before = g_native_calls;
    Check(internal::ReadSetting(0, internal::kPresetSetting, &value, caller), "explicit preset read");
    Check(value == static_cast<uint32_t>(choice) && g_native_calls == before, "supplies only selected A/B");
    Check(preset::g_last_supplied == static_cast<int>(choice), "records actual supplied value");
  }
  preset::g_requested = preset::Preset::kA;
  Check(preset::g_last_supplied == 2, "editing requested does not fabricate observed A");
  g_native_value = 19;
  Check(internal::ReadSetting(0, 0x104D6667, &value, caller) && value == 19, "frame count setting untouched");
  Check(internal::ReadSetting(0, 0x10E41DF3, &value, caller) && value == 19, "SR preset untouched");
  Check(internal::ReadSetting(0, internal::kPresetSetting, &value, nullptr) && value == 19,
        "other callers, including override-state reporting, untouched");
  Check(preset::g_last_supplied == 2, "unrelated queries leave evidence unchanged");
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

void ExactProvider(const char* path) {
  auto image = MapProvider(path);
  auto module = reinterpret_cast<HMODULE>(image.data());
  provider_mock::regions.push_back({module, image.size(), MEM_IMAGE, PAGE_EXECUTE_READ});
  Check(internal::MatchesProvider(module), "real 310.9.1 reader, A/B consumer and reporter fingerprints");
  Check(!internal::MatchesProvider(reinterpret_cast<HMODULE>(reinterpret_cast<uintptr_t>(module) | 1u)),
        "resource-only handle rejected");
  for (auto rva : {internal::kReaderRva, internal::kSelectionRva, internal::kReportOverridesRva}) {
    image[rva] ^= 1;
    Check(!internal::MatchesProvider(module), "modified machine code rejected despite matching metadata");
    image[rva] ^= 1;
  }
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
  --nt->FileHeader.TimeDateStamp;
  Check(!internal::MatchesProvider(module), "other build rejected");
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
    Check(preset::TryInstall(copies[i].data()) == (i < 3), "bounded independent provider hook slots");
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
  if (argc == 2) ExactProvider(argv[1]);
  std::cout << "fg_preset_test: " << g_checks << " checks passed";
  if (argc == 2) std::cout << " (including real provider fingerprints; mocked hooks, no GPU)";
  std::cout << '\n';
}
