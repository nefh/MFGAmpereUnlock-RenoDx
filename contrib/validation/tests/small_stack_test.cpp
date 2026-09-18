// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/loadhook.hpp"
#include "../../../src/addons/mfgunlock/provider.hpp"
#include "../../../src/addons/mfgunlock/quality_config.hpp"
#include "../../../src/addons/mfgunlock/diagnostic_bridge.hpp"
#include <process.h>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace hook = mfgunlock::hook;
using Target = int (*)(int);
Target g_original = nullptr;
hook::AddressHook g_address;

// Exported, nontrivial prologue: this exercises real Detours, not an API double.
extern "C" __declspec(dllexport) __declspec(noinline) int MfgStackTarget(int value) {
  volatile int result = value;
  result = result * 3;
  result = result + 7;
  return result;
}
int Replace(int value) { return g_original(value) + 1; }
int ReplaceAddress(int value) { return g_address.Original<Target>()(value) + 2; }

unsigned int __stdcall Run(void* argument) {
  const auto requested = reinterpret_cast<uintptr_t>(argument);
  char marker;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(&marker, &memory, sizeof(memory))) return 1;
  const auto high = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
  const auto reserve = high - reinterpret_cast<uintptr_t>(memory.AllocationBase);
  std::printf("stack requested=%zu reserved=%zu\n", static_cast<size_t>(requested), static_cast<size_t>(reserve));
  if (reserve != requested) return 2;
  try {
    namespace qc = mfgunlock::qualityconfig;
    qc::g_requested = qc::Encode(qc::Config{});
    qc::g_prepared = qc::kUnknown;
    const auto frozen = qc::Freeze();
    qc::g_requested.fetch_xor(2);
    if (!qc::Pending(qc::g_requested.load(), frozen) || qc::Freeze() != frozen) return 14;
    mfgunlock::diagnostic::LifecycleEvent event;
    event.kind = mfgunlock::diagnostic::LifecycleKind::kQualityRequested;
    mfgunlock::diagnostic::Emit(event);
    HMODULE module = GetModuleHandleW(nullptr);
    mfgunlock::provider::internal::Image image;
    if (!mfgunlock::provider::internal::InspectImage(module, image)) return 3;
    if (mfgunlock::provider::internal::ModulePath(module).empty()) return 4;
    mfgunlock::loadhook::internal::Notify(module, nullptr);
    if (mfgunlock::provider::IsDlssgProvider(module)) return 5;

    const std::vector<hook::HookItem> hooks = {
        {"MfgStackTarget", reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Replace)}};
    void* missing = nullptr;
    auto incomplete = hooks;
    incomplete.push_back({"MfgMissingExport", &missing, reinterpret_cast<void*>(&Replace)});
    if (hook::Install(module, incomplete, "stack preflight") || g_original || missing) return 6;
    if (!hook::Install(module, hooks, "stack install")) return 7;
    const auto target = reinterpret_cast<Target>(GetProcAddress(module, "MfgStackTarget"));
    if (!target || target(4) != 20) return 8;
    hook::Uninstall(hooks);
    if (target(4) != 19) return 9;
    if (!hook::InstallAddress(g_address, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&ReplaceAddress), "stack address")) return 10;
    if (target(4) != 21) return 11;
    hook::UninstallAddress(g_address);
    if (g_address.installed.load() || target(4) != 19) return 12;
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL stack preflight: %s\n", error.what());
    return 13;
  }
}

int main(int argc, char** argv) {
  const unsigned kib = argc == 2 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 0;
  if (kib != 64 && kib != 128) return 1;
  const unsigned bytes = kib * 1024;
  // Without RESERVATION the default megabyte reserve hides large-stack bugs.
  const auto raw = _beginthreadex(nullptr, bytes, Run, reinterpret_cast<void*>(static_cast<uintptr_t>(bytes)),
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!raw) { std::fprintf(stderr, "FAIL creating %u KiB thread\n", kib); return 1; }
  HANDLE thread = reinterpret_cast<HANDLE>(raw);
  if (WaitForSingleObject(thread, 15000) != WAIT_OBJECT_0) {
    std::fprintf(stderr, "FAIL timeout on %u KiB stack\n", kib);
    CloseHandle(thread);
    return 1;
  }
  DWORD result = 1;
  GetExitCodeThread(thread, &result);
  CloseHandle(thread);
  std::printf("%s small stack %u KiB (result=%lu)\n", result == 0 ? "PASS" : "FAIL", kib, result);
  return result == 0 ? 0 : 1;
}
