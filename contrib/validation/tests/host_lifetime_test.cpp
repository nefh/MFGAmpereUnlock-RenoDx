// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/addon_entry.hpp"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#endif
namespace {
unsigned checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
}
int main(int argc, char** argv) {
  try {
    using mfgunlock::addonentry::HostLifetime;
    int host_data = 1, other_data = 2;
    void* host = &host_data;
    unsigned calls = 0;
    HostLifetime retained;
    Check(!retained.Ensure(nullptr, [&](void*) { ++calls; return true; }), "null host");
    Check(calls == 0 && retained.Host() == nullptr, "null host does not invoke platform");
    Check(!retained.Ensure(host, [&](void*) { ++calls; return false; }), "pin failure");
    Check(retained.Host() == nullptr, "failure does not poison later registration");
    bool mapped = true, pinned = false;
    Check(retained.Ensure(host, [&](void* candidate) {
      ++calls; pinned = candidate == host && mapped; return pinned;
    }), "pin before caching SDK addresses");
    for (unsigned i = 0; i < 256; ++i) {
      if (!pinned) mapped = false;  // Last ordinary reference from a Vulkan probe.
      Check(mapped, "cached logger target must remain mapped");
      Check(retained.Ensure(host, [&](void*) { ++calls; return true; }), "idempotent same host");
    }
    Check(calls == 2, "no repeated retention on host-manager refresh");
    Check(!retained.Ensure(&other_data, [&](void*) { ++calls; return true; }), "no cross-host cache rebinding");
    Check(calls == 2 && retained.Host() == host, "other host leaves retained identity intact");
#ifdef _WIN32
    Check(argc == 2, "native fixture path required");
    const auto path = std::filesystem::absolute(std::filesystem::path(argv[1])).make_preferred().wstring();
    HMODULE fixture = LoadLibraryW(path.c_str());
    Check(fixture != nullptr, "load native test host");
    auto call = reinterpret_cast<int (*)()>(GetProcAddress(fixture, "HostLifetimeFixture"));
    Check(call && call() == 317, "native export before release");
    Check(FreeLibrary(fixture) != 0, "release unpinned control");
    Check(GetModuleHandleW(path.c_str()) == nullptr, "control host really unloads");
    fixture = LoadLibraryW(path.c_str());
    Check(fixture != nullptr, "reload test host");
    call = reinterpret_cast<int (*)()>(GetProcAddress(fixture, "HostLifetimeFixture"));
    HostLifetime native;
    Check(native.Ensure(fixture, [](void* candidate) {
      HMODULE h = nullptr;
      return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          reinterpret_cast<LPCWSTR>(candidate), &h) && h == candidate;
    }), "native Windows pin succeeds");
    Check(FreeLibrary(fixture) != 0, "release original native host reference");
    Check(GetModuleHandleW(path.c_str()) == fixture, "pinned host survives last ordinary release");
    Check(call && call() == 317, "actual cached entrypoint stays callable after release");
    std::cout << "Windows native LoadLibrary/FreeLibrary retention PASS\n";
#else
    (void)argc; (void)argv;
    std::cout << "Windows native host retention NOT_RUN (portable model only)\n";
#endif
    std::cout << "host_lifetime_test: " << checks << " checks PASS\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
