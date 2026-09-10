// SPDX-License-Identifier: MIT
// Runs the production provider registry against simulated Windows memory and
// loader operations. No NVIDIA DLL is loaded and no GPU code is executed.

#include "../../../src/addons/mfgunlock/ampere.hpp"
#include "../../../src/addons/mfgunlock/ampere_policy.hpp"

#include <iostream>
#include <stdexcept>

namespace ampere = mfgunlock::ampere;
namespace mock = provider_mock;

using Bytes = std::vector<unsigned char>;

namespace {

unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (!condition) throw std::runtime_error(reason);
}

template <class T>
void Put(Bytes& bytes, size_t offset, const T& value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

struct Image {
  Bytes bytes = Bytes(0x3000);
  HMODULE module = bytes.data();

  explicit Image(unsigned int id, bool valid_gate = true) {
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    Put(bytes, 0, dos);

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 2;
    nt.FileHeader.TimeDateStamp = id;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(bytes.size());
    nt.OptionalHeader.SizeOfHeaders = 0x400;
    Put(bytes, 0x80, nt);

    IMAGE_SECTION_HEADER text{};
    text.VirtualAddress = 0x1000;
    text.Misc.VirtualSize = 0x1000;
    text.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;

    IMAGE_SECTION_HEADER data{};
    data.VirtualAddress = 0x2000;
    data.Misc.VirtualSize = 0x1000;
    data.Characteristics = IMAGE_SCN_MEM_READ;

    Put(bytes, 0x80 + sizeof(nt), text);
    Put(bytes, 0x80 + sizeof(nt) + sizeof(text), data);

    const unsigned char gate[] = {0xb8, 0x90, 1, 0, 0, 0xc3};
    std::memcpy(bytes.data() + 0x1000, gate, sizeof(gate));
    if (!valid_gate) bytes[0x1001] = 0xb0;

    // One minimal compressed PTX container. This is the same validated layout
    // used by ampere_ptx_test.
    std::string ptx =
        ".version 8.7\n"
        ".target sm_89\n"
        ".address_size 64\n"
        ".visible .entry test() { ret; }\n";
    ptx.push_back('\0');

    Bytes payload{0xf0, static_cast<unsigned char>(ptx.size() - 15)};
    payload.insert(payload.end(), ptx.begin(), ptx.end());

    Bytes fatbin(80 + ((payload.size() + 7) & ~size_t{7}));
    Put<uint32_t>(fatbin, 0, mfgunlock::fatbin::kMagic);
    Put<uint16_t>(fatbin, 4, 1);
    Put<uint16_t>(fatbin, 6, 16);
    Put<uint64_t>(fatbin, 8, fatbin.size() - 16);
    Put<uint16_t>(fatbin, 16, 1);
    Put<uint16_t>(fatbin, 18, 0x101);
    Put<uint32_t>(fatbin, 20, 64);
    Put<uint64_t>(fatbin, 24, fatbin.size() - 80);
    Put<uint32_t>(fatbin, 32, static_cast<uint32_t>(payload.size()));
    Put<uint32_t>(fatbin, 44, 89);
    Put<uint64_t>(fatbin, 56, 0x2041);
    Put<uint64_t>(fatbin, 72, ptx.size());
    std::copy(payload.begin(), payload.end(), fatbin.begin() + 80);
    std::copy(fatbin.begin(), fatbin.end(), bytes.begin() + 0x2000);

    mock::regions.push_back({module, bytes.size()});
    mock::paths[module] = "X:\\fixture\\provider_" + std::to_string(id) + "\\nvngx_dlssg.dll";
    mock::exports[{module, "NVSDK_NGX_GetGPUArchitecture"}] =
        reinterpret_cast<FARPROC>(bytes.data() + 0x1000);
    mock::exports[{module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"}] =
        reinterpret_cast<FARPROC>(bytes.data() + 0x1100);
  }

  void Unmap() {
    std::erase_if(mock::regions, [this](const auto& region) { return region.base == module; });
  }
};

void Reset() {
  ampere::internal::g_providers.clear();
  ampere::internal::g_rejected.clear();
  ampere::internal::g_registry_failed = false;
  ampere::internal::g_ignored_mappings = 0;

  mfgunlock::g_enabled = true;
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kAmpere);
  ampere::g_create_seen = false;

  mock::regions.clear();
  mock::exports.clear();
  mock::paths.clear();
  mock::retain_ok = true;
  mock::force_lock_busy = false;
  mock::protection_calls = 0;
  mock::fail_protection_call = 0;
  reshade::log::lines.clear();
}

bool Allowed() {
  return ampere::CanRelaxRequirements({true, true, ampere::PreparedProviderCount(),
                                       ampere::kNgxSuccess, ampere::kDlssGFeatureId,
                                       ampere::kAdapterUnsupported, ampere::kAdaArchitecture},
                                      mfgunlock::architecture::ActiveProfile());
}

}  // namespace

int main() {
  try {
    Reset();
    Image ready(1);
    const auto original = ready.bytes;
    Check(ampere::PrepareProvider(ready.module), "initial provider preparation");
    Check(ampere::PreparedProviderCount() == 1 && Allowed(),
          "single prepared provider admitted");
    Check(ready.bytes[0x1001] == 0x70, "minimum arch written last");

    auto writes = mock::protection_calls;
    Check(ampere::PrepareProvider(ready.module) && mock::protection_calls == writes,
          "preparation idempotent");

    Image rejected(2, false);
    Check(!ampere::PrepareProvider(rejected.module), "unrecognized genuine provider rejected");
    Check(ampere::GetProviderStatus().ready == 1 && ampere::GetProviderStatus().blocked == 1 &&
              !Allowed(),
          "live rejected provider vetoes");
    Check(!ampere::internal::g_rejected.front().path.empty() &&
              !ampere::internal::g_rejected.front().reason.empty(),
          "rejection identifies module and reason");

    rejected.Unmap();
    Check(ampere::GetProviderStatus().ready == 1 && ampere::GetProviderStatus().blocked == 0 &&
              ampere::GetProviderStatus().expired_rejections == 1 && Allowed(),
          "expired rejection does not poison ready provider");

    // A historical rejection must not veto a provider that is currently qualified.
    const unsigned int old_count = ampere::internal::g_rejected.empty() ? 1u : 0u;
    Check(old_count == 0 && ampere::PreparedProviderCount() == 1,
          "historical global veto is distinguishable from the live registry");
    Check(ampere::PrepareProvider(ready.module),
          "historical rejection cannot block idempotent reuse");

    auto tagged = reinterpret_cast<HMODULE>(reinterpret_cast<uintptr_t>(ready.module) | 1u);
    writes = mock::protection_calls;
    Check(!ampere::PrepareProvider(tagged) && mock::protection_calls == writes && Allowed(),
          "tagged resource ignored without writes");

    Image data(3);
    for (auto& region : mock::regions) {
      if (region.base == data.module) region.type = MEM_MAPPED;
    }
    Check(!ampere::PrepareProvider(data.module) && Allowed(),
          "data mapping ignored without global veto");

    Image nonprovider(4);
    mock::exports.erase({nonprovider.module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"});
    mock::exports.erase({nonprovider.module, "NVSDK_NGX_GetGPUArchitecture"});
    Check(!ampere::PrepareProvider(nonprovider.module) && Allowed(),
          "filename-only false positive ignored");

    Image forwarded(5);
    mock::exports[{forwarded.module, "NVSDK_NGX_GetGPUArchitecture"}] =
        reinterpret_cast<FARPROC>(ready.bytes.data() + 0x1000);
    Check(!ampere::PrepareProvider(forwarded.module) && !Allowed(),
          "unresolved forwarded ABI blocks, never patched");
    forwarded.Unmap();

    Image incomplete(13);
    mock::exports.erase({incomplete.module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"});
    Check(!ampere::PrepareProvider(incomplete.module) && !Allowed(),
          "partial provider ABI is a real blocker");

    auto* reused_header = reinterpret_cast<IMAGE_NT_HEADERS64*>(incomplete.bytes.data() + 0x80);
    ++reused_header->FileHeader.TimeDateStamp;
    Check(ampere::GetProviderStatus().ready == 1 && ampere::GetProviderStatus().blocked == 1 &&
              !Allowed(),
          "new image at rejected address cannot authorize the other ready provider");
    incomplete.Unmap();

    Image retry(6);
    mock::retain_ok = false;
    Check(!ampere::PrepareProvider(retry.module) && !Allowed(),
          "unretained real provider blocks");
    mock::retain_ok = true;
    Check(ampere::PrepareProvider(retry.module), "same provider can be fully requalified");
    Check(ampere::GetProviderStatus().blocked == 0 && ampere::GetProviderStatus().ready == 2 &&
              !Allowed(),
          "two prepared providers remain ambiguous");
    Check(ampere::internal::g_rejected.size() == 3,
          "only successful transaction retires its own rejection");

    ampere::Restore();
    Check(ready.bytes == original, "restore original provider bytes");

    Reset();
    Image failing(7);
    mock::fail_protection_call = 2;
    const auto clean = failing.bytes;
    Check(!ampere::PrepareProvider(failing.module),
          "post-write protection failure rejects transaction");
    Check(failing.bytes == clean && ampere::GetProviderStatus().blocked == 1 && !Allowed(),
          "rollback verified and failed transaction blocks");
    Check(!ampere::internal::g_providers.front().failure.empty(),
          "transaction failure has diagnostic");

    Reset();
    Image mismatch(8);
    Check(ampere::PrepareProvider(mismatch.module), "prepare for invalidation test");
    mismatch.bytes[0x1001] = 0x90;
    Check(ampere::GetProviderStatus().blocked == 1 && !Allowed(),
          "external gate modification cannot pass");

    Reset();
    Image reuse(9, false);
    Check(!ampere::PrepareProvider(reuse.module), "identity old generation rejected");
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reuse.bytes.data() + 0x80);
    ++nt->FileHeader.TimeDateStamp;
    Check(ampere::GetProviderStatus().expired_rejections == 0 &&
              ampere::GetProviderStatus().blocked == 1 && !Allowed(),
          "reused address stays unqualified, not an expired veto");
    reuse.bytes[0x1001] = 0x90;
    Check(ampere::PrepareProvider(reuse.module) && Allowed(),
          "remapped candidate must pass full preparation");
    Check(ampere::internal::g_rejected.empty(),
          "successful new generation clears old rejection");

    mock::force_lock_busy = true;
    Check(ampere::GetProviderStatus().busy && !Allowed(), "registry lock contention fails closed");
    mock::force_lock_busy = false;

    Reset();
    Image bad(10);
    bad.bytes[0] = 0;
    Check(!ampere::PrepareProvider(bad.module) && ampere::GetProviderStatus().failed && !Allowed(),
          "malformed executable never silently ignored");

    Reset();
    Image late(11);
    ampere::g_create_seen = true;
    Check(!ampere::PrepareProvider(late.module) && !Allowed(),
          "new provider after Create refused");

    Reset();
    Image disabled(12);
    mfgunlock::g_enabled = false;
    Check(!ampere::PrepareProvider(disabled.module) && mock::protection_calls == 0,
          "disabled Ampere path makes no writes");

    for (auto selected : {mfgunlock::Architecture::kAmpere, mfgunlock::Architecture::kTuring}) {
      Reset();
      mfgunlock::architecture::Configure(selected);
      const auto* profile = mfgunlock::architecture::ActiveProfile();
      Image target(100);
      const auto before = target.bytes;
      Check(ampere::PrepareProvider(target.module), "explicit profile prepares provider");
      Check(target.bytes[0x1001] == static_cast<unsigned char>(profile->native_arch),
            "minimum architecture follows profile");
      Check(mfgunlock::fatbin::ReadU32(target.bytes.data() + 0x2000 + 44) == profile->target_sm,
            "fatbin SM follows profile");
      target.bytes[0x1100] = 0xb0;
      target.bytes[0x1101] = 0xb0;
      unsigned char* sites[] = {target.bytes.data() + 0x1100, target.bytes.data() + 0x1101};
      Check(ampere::ApplyMfgComparisons(sites), "profile MFG comparison transaction");
      Check(*sites[0] == static_cast<unsigned char>(profile->native_arch) &&
                *sites[1] == static_cast<unsigned char>(profile->native_arch),
            "MFG comparisons use native target, not spoofed Ada");
      // Comparison sites are owned/restored by addon.cpp, not the provider transaction.
      *sites[0] = before[0x1100];
      *sites[1] = before[0x1101];
      ampere::Restore();
      Check(target.bytes == before, "profile restore is byte exact");
    }

    Reset();
    mfgunlock::architecture::Configure(mfgunlock::Architecture::kAda);
    Image ada(101);
    const auto ada_before = ada.bytes;
    Check(!ampere::PrepareProvider(ada.module) && ada.bytes == ada_before &&
              mock::protection_calls == 0,
          "Ada skips backport preparation");

    Reset();
    mfgunlock::architecture::Configure(mfgunlock::Architecture::kAuto);
    Image automatic(102);
    const auto automatic_before = automatic.bytes;
    Check(!ampere::PrepareProvider(automatic.module) && automatic.bytes == automatic_before,
          "pending Auto writes nothing");
    mfgunlock::architecture::ResolveAuto(0x10de, 0x160, 2);
    Check(ampere::PrepareProvider(automatic.module) && automatic.bytes[0x1001] == 0x60,
          "resolved Auto uses Turing provider target");
    ampere::Restore();
    Check(automatic.bytes == automatic_before, "Auto provider restored");

    std::cout << "provider state: " << g_checks
              << " checks PASS (real ampere.hpp; simulated Windows memory)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
