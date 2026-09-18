// SPDX-License-Identifier: MIT
// Compiles the actual ngx_hook.hpp, not the host-wrapper replacement header.
#include "../../../src/addons/mfgunlock/ngx_hook.hpp"
#include <cstdio>
#include <array>
#include <stdexcept>
namespace hook = mfgunlock::hook;
namespace mock = detour_mock;
namespace {
unsigned int g_checks = 0;
hook::AddressHook g_entry;
void Check(bool ok, const char* reason) {
  ++g_checks;
  if (!ok) throw std::runtime_error(reason);
}
void Target() {}
void Replacement() {}
void Reset() {
  mock::Reset();
  hook_mock::snapshot_ok = hook_mock::enumerate_ok = hook_mock::open_ok = true;
}
}
int main() {
  try {
    alignas(8) std::array<unsigned char, 1024> image{};
    HMODULE module = image.data();
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 64;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 64);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    provider_mock::regions.push_back({module, image.size()});
    provider_mock::exports[{module, "first"}] = Target;
    void* first = nullptr;
    void* second = nullptr;
    const std::vector<hook::HookItem> hooks = {
        {"first", &first, reinterpret_cast<void*>(Replacement)},
        {"second", &second, reinterpret_cast<void*>(Replacement)}};
    Check(!hook::Install(module, hooks, "fixture"), "missing second export aborts preflight");
    Check(first == nullptr && second == nullptr && mock::begins == 0,
          "missing export publishes no partial pointers");
    provider_mock::exports[{module, "second"}] = Target;
    for (auto failure : {mock::Failure::kBegin, mock::Failure::kUpdate,
                         mock::Failure::kAttach, mock::Failure::kCommit}) {
      Reset();
      mock::failure = failure;
      Check(!hook::Install(module, hooks, "fixture"), "transaction failure propagated");
      Check(!first && !second && !mock::transaction, "failed hook leaves no poisoned pointers/transaction");
      mock::failure = mock::Failure::kNone;
      Check(hook::Install(module, hooks, "fixture"), "retry succeeds after failure");
      hook::Uninstall(hooks);
      first = second = nullptr;
    }
    for (bool* flag : {&hook_mock::snapshot_ok, &hook_mock::enumerate_ok, &hook_mock::open_ok}) {
      Reset(); *flag = false;
      Check(!hook::Install(module, hooks, "fixture") && !first && !second && mock::begins == 0,
            "thread discovery failure does not masquerade as zero threads");
    }
    Reset();
    Check(hook::Install(module, hooks, "fixture"), "install before failed detach");
    mock::failure = mock::Failure::kUpdate;
    hook::Uninstall(hooks);
    Check(mock::detaches == 0 && mock::aborts == 1, "failed thread enlistment prevents detach writes");
    Reset();
    void* address = image.data() + 512;
    mock::failure = mock::Failure::kCommit;
    Check(!hook::InstallAddress(g_entry, address, reinterpret_cast<void*>(Replacement), "entry"),
          "address commit failure propagated");
    Check(!g_entry.trampoline.load() && !g_entry.target.load() && !g_entry.installed.load(),
          "address commit failure clears published state");
    mock::failure = mock::Failure::kNone;
    mock::during_commit = [] {
      Check(g_entry.trampoline.load() != nullptr, "original published before commit resumes callers");
    };
    Check(hook::InstallAddress(g_entry, address, reinterpret_cast<void*>(Replacement), "entry"), "address retry succeeds");
    mock::during_commit = nullptr;
    mock::failure = mock::Failure::kUpdate;
    hook::UninstallAddress(g_entry);
    Check(g_entry.installed.load(), "failed address detach retains usable state");
    mock::failure = mock::Failure::kNone;
    hook::UninstallAddress(g_entry);
    Check(!g_entry.installed.load() && !g_entry.trampoline.load(), "address detach retry succeeds");
    std::printf("PASS hook lifecycle: %u checks\n", g_checks);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL hook lifecycle: %s\n", error.what());
    return 1;
  }
}
