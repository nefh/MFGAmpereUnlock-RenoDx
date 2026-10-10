// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/provider.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string_view>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
constexpr DWORD MAX_PATH = 260;  // Provider API double does not define this Windows constant.
constexpr char kFlipMarker[] = "FG1 DLL has been detected";
std::atomic_bool g_flip_meter_failed{false}, g_flip_meter_patched{false};
std::vector<int> g_flip_meter_sites;
void PatchFrameCountCeiling(HMODULE) {}  // Tested by the independent ceiling path.
bool WriteFlipSite(unsigned char*, const unsigned char*, size_t) {
  g_flip_meter_sites.push_back(1);  // Selection test; do not execute image writes.
  return true;
}
#include "addon_image_seam.hpp"

int main(int argc, char** argv) {
  // The corpus runner supplies an offline mapped-layout byte copy through
  // stdin. API doubles query that copy; no DLL is mapped by the native loader,
  // called, or modified. Reuse the exact production scanner above.
  if (argc == 2 && std::string_view(argv[1]) == "--inspect-stdin") {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::vector<unsigned char> image;
    std::array<char, 65536> chunk{};
    while (std::cin) {
      std::cin.read(chunk.data(), chunk.size());
      const size_t count = static_cast<size_t>(std::cin.gcount());
      if (image.size() + count > 128u * 1024u * 1024u) return 1;
      image.insert(image.end(), chunk.data(), chunk.data() + count);
    }
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return 1;
    HMODULE module = image.data();
    provider_mock::regions = {{module, image.size()}};
    unsigned char* base = nullptr;
    const IMAGE_NT_HEADERS64* headers = nullptr;
    const bool valid = ModuleImage(module, &base, &headers);
    size_t markers = 0;
    const unsigned char* marker = nullptr;
    if (valid) {
      const auto* section = IMAGE_FIRST_SECTION(headers);
      for (WORD i = 0; i < headers->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        for (size_t off = 0; off + sizeof(kFlipMarker) - 1 <= section->Misc.VirtualSize; ++off) {
          const auto* candidate = base + section->VirtualAddress + off;
          if (std::memcmp(candidate, kFlipMarker, sizeof(kFlipMarker) - 1) == 0) {
            marker = candidate;
            ++markers;
          }
        }
      }
    }
    unsigned int field = 0;
    int value = -1;
    const bool unique = markers == 1 &&
        FindFlipMeteringFallback(base, headers, marker, field, value);
    if (valid) TryPatchFlipMeteringInModule(module);
    std::cout << "{\"image_valid\":" << (valid ? "true" : "false")
              << ",\"marker_matches\":" << markers
              << ",\"unique_fallback\":" << (unique ? "true" : "false")
              << ",\"field_offset_observation\":" << field
              << ",\"fallback_value\":" << value
              << ",\"planned_store_count\":" << g_flip_meter_sites.size()
              << ",\"writes_executed\":false,\"runtime_authorized\":false}\n";
    return 0;
  }
  if (argc != 1) return 1;
  alignas(16) std::array<unsigned char, 0x3000> bytes{};
  HMODULE mod = bytes.data();
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + 0x80);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x80;
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
  nt->FileHeader.NumberOfSections = 2;
  nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  nt->OptionalHeader.SizeOfHeaders = 0x400;
  nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(bytes.size());
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(bytes.data() + 0x80 + sizeof(*nt));
  section[0].VirtualAddress = 0x1000;
  section[0].Misc.VirtualSize = 0x1000;
  section[0].Characteristics = IMAGE_SCN_MEM_EXECUTE;
  section[1].VirtualAddress = 0x2000;
  section[1].Misc.VirtualSize = 0x1000;
  section[1].Characteristics = IMAGE_SCN_MEM_READ;
  const auto pristine = bytes;
  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* headers = nullptr;
  provider_mock::regions = {{mod, bytes.size()}};
  assert(ModuleImage(mod, &base, &headers));
  assert(base == bytes.data() && headers == nt);
  auto reject = [&] {
    base = bytes.data(); headers = nt;
    assert(!ModuleImage(mod, &base, &headers));
    assert(base == nullptr && headers == nullptr);
    bytes = pristine;
    provider_mock::regions = {{mod, bytes.size()}};
  };
  dos->e_lfanew = -1; reject();
  dos->e_lfanew = 0x7fffffff; reject();
  dos->e_magic = 0; reject();
  nt->FileHeader.Machine = 0; reject();
  nt->FileHeader.NumberOfSections = 97; reject();
  nt->FileHeader.SizeOfOptionalHeader = 1; reject();
  nt->OptionalHeader.SizeOfHeaders = 0x100; reject();
  nt->OptionalHeader.SizeOfImage = 0x1000; reject();
  section[1].VirtualAddress = 0x1800; reject();
  section[1].Misc.VirtualSize = 0xffffffffu; reject();
  // A mapped header/section table does not imply readable section pages.
  provider_mock::regions.front().bytes = 0x400; reject();
  provider_mock::regions.front().bytes = 0x80 + sizeof(*nt) + sizeof(*section); reject();
  provider_mock::regions.front().protect = PAGE_NOACCESS; reject();
  provider_mock::regions.front().protect |= PAGE_GUARD; reject();
  provider_mock::regions.front().type = 0; reject();
  assert(!ModuleImage(nullptr, &base, &headers));
  assert(!ModuleImage(mod, nullptr, &headers));
  assert(!ModuleImage(mod, &base, nullptr));
  assert(provider_mock::protection_calls == 0);
  // The optional flip patch must reject ambiguity before any selected write.
  auto* marker = bytes.data() + 0x2200;
  std::memcpy(marker, kFlipMarker, sizeof(kFlipMarker));
  unsigned int offset = 0;
  int value = -1;
  assert(!FindFlipMeteringFallback(bytes.data(), nt, marker, offset, value));
  const unsigned char lea[] = {0x48, 0x8d, 0x0d};
  std::memcpy(bytes.data() + 0x1000, lea, sizeof(lea));
  const int displacement = 0x2200 - 0x1007;
  std::memcpy(bytes.data() + 0x1003, &displacement, sizeof(displacement));
  const unsigned char fallback[] = {0xc6, 0x80, 0x34, 0x12, 0, 0, 0};
  std::memcpy(bytes.data() + 0x1010, fallback, sizeof(fallback));
  assert(FindFlipMeteringFallback(bytes.data(), nt, marker, offset, value));
  assert(offset == 0x1234 && value == 0);
  bytes[0x1011] = 0x88;  // Not C6 /0.
  assert(!FindFlipMeteringFallback(bytes.data(), nt, marker, offset, value));
  bytes[0x1011] = 0x84;  // SIB: the displacement is not in the assumed location.
  assert(!FindFlipMeteringFallback(bytes.data(), nt, marker, offset, value));
  bytes[0x1011] = 0x80;
  std::memcpy(bytes.data() + 0x1020, fallback, sizeof(fallback));
  assert(!FindFlipMeteringFallback(bytes.data(), nt, marker, offset, value));
  assert(TryPatchFlipMeteringInModule(mod) && g_flip_meter_sites.empty());
  assert(g_flip_meter_failed);
  std::memset(bytes.data() + 0x1020, 0, sizeof(fallback));
  std::memcpy(bytes.data() + 0x2300, kFlipMarker, sizeof(kFlipMarker));
  g_flip_meter_failed = false;
  assert(TryPatchFlipMeteringInModule(mod) && g_flip_meter_sites.empty());
  assert(g_flip_meter_failed);
  std::memset(bytes.data() + 0x2300, 0, sizeof(kFlipMarker));
  // One uniquely derived fallback, one opposite store outside its search window.
  std::memcpy(bytes.data() + 0x1400, fallback, sizeof(fallback));
  bytes[0x1406] = 1;
  g_flip_meter_failed = false;
  assert(TryPatchFlipMeteringInModule(mod));
  assert(g_flip_meter_sites.size() == 1 && g_flip_meter_patched);
  assert(provider_mock::protection_calls == 0);
}
