// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgdiagnostics/mfg_probe.hpp"
#include <cstdio>
#include <stdexcept>
#include <type_traits>
namespace probe = mfgdiagnostics::probe;
namespace count = mfgunlock::countobserver;
namespace {
unsigned int g_checks = 0, g_calls = 0, g_events = 0;
int64_t g_value = 0;
bool g_recording = true;
NVSDK_NGX_Result g_result = NVSDK_NGX_Result_Success;
count::Event g_last;
void Check(bool ok, const char* name) { ++g_checks; if (!ok) throw std::runtime_error(name); }
void Sink(const count::Event* event) noexcept { ++g_events; g_last = *event; }
void SetU(NVSDK_NGX_Parameter*, const char*, unsigned int value) { ++g_calls; g_value = value; }
void SetI(NVSDK_NGX_Parameter*, const char*, int value) { ++g_calls; g_value = value; }
NVSDK_NGX_Result GetU(NVSDK_NGX_Parameter*, const char*, unsigned int* value) {
  ++g_calls; if (value && g_result == NVSDK_NGX_Result_Success) *value = 5; return g_result;
}
NVSDK_NGX_Result GetI(NVSDK_NGX_Parameter*, const char*, int* value) {
  ++g_calls; if (value && g_result == NVSDK_NGX_Result_Success) *value = -1; return g_result;
}
}
int main() {
  try {
    static_assert(std::is_standard_layout_v<count::Event> && std::is_trivially_copyable_v<count::Event>);
    probe::g_sink = Sink;
    probe::g_recording = [] { return g_recording; };
    auto& slot = probe::g_slots[0];
    slot.set_unsigned = SetU; slot.set_signed = SetI;
    slot.get_unsigned = GetU; slot.get_signed = GetI;
    NVSDK_NGX_Parameter parameter;
    probe::Set<0, unsigned int>(&parameter, "DLSSG.MultiFrameCount", 5);
    Check(g_calls == 1 && g_events == 1 && g_value == 5, "Set forwarded exactly once unchanged");
    Check(g_last.result == count::kUnknown && g_last.value_known && g_last.value == 5,
          "void Set is not falsely reported as accepted");
    Check(g_last.caller != 0 && g_last.origin == count::Origin::kUnknown,
          "C caller observed without inventing override provenance");
    probe::Set<0, int>(&parameter, "DLSSG.MultiFrameCountMax", -1);
    Check(g_last.value == -1 && g_value == -1, "signed values not reinterpreted as unsigned");
    unsigned int value = 0;
    Check(probe::Get<0, unsigned int>(&parameter, "DLSSG.MultiFrameCountMax", &value) == g_result,
          "Get preserves return code");
    Check(value == 5 && g_last.value == 5 && g_last.value_known, "Get logs returned value");
    g_result = NVSDK_NGX_Result_FAIL;
    value = 0xdeadbeef;
    const auto calls = g_calls;
    Check(probe::Get<0, unsigned int>(&parameter, "DLSSG.MultiFrameCountMax", &value) == g_result,
          "Get preserves vendor error");
    Check(g_calls == calls + 1 && value == 0xdeadbeef && !g_last.value_known,
          "failed Get neither overwrites nor reads invalid output");
    const auto events = g_events;
    probe::Set<0, int>(&parameter, "Unrelated", 10);
    Check(g_events == events && g_value == 10, "unrelated parameter passed through unrecorded");
    g_recording = false;
    probe::Set<0, int>(&parameter, "DLSSG.MultiFrameIndex", 2);
    Check(g_events == events && g_value == 2, "disabled capture has no count records");
    g_recording = true;
    probe::g_recording = [] { SetLastError(111); return g_recording; };
    probe::g_sink = [](const count::Event*) noexcept { SetLastError(222); };
    SetLastError(333);
    probe::Set<0, unsigned int>(&parameter, "DLSSG.MultiFrameCount", 3);
    Check(GetLastError() == 333, "count observation preserves the forwarded API LastError");
    probe::g_sink = Sink;
    count::g_callback = Sink;
    count::g_context = {100, 1};
    {
      count::Scope outer(200, 3);
      { count::Scope inner(300, 5); Check(count::g_context.caller == 300, "nested context installed"); }
      Check(count::g_context.caller == 200 && count::g_context.requested == 3, "nested context restores caller");
    }
    Check(count::g_context.caller == 100, "outer context restored");
    count::g_callback = nullptr;
    { count::Scope disabled(400, 5); Check(count::g_context.caller == 100, "no TLS rewrite without observer"); }
    probe::g_slots = {};
    probe::g_hook_count = 0;
    detour_mock::Reset();
    const auto module = reinterpret_cast<HMODULE>(uintptr_t{0x10000});
    provider_mock::exports[{module, probe::kExports[0]}] = reinterpret_cast<FARPROC>(SetU);
    provider_mock::exports[{module, probe::kExports[2]}] = reinterpret_cast<FARPROC>(SetU);
    probe::Install(module);
    Check(probe::g_hook_count == 0 && detour_mock::attaches == 0,
          "ambiguous Set/Get export aliases are both left unhooked");
    provider_mock::exports[{module, probe::kExports[2]}] = reinterpret_cast<FARPROC>(GetU);
    probe::Install(module);
    Check(probe::g_hook_count == 2 && detour_mock::attaches == 2,
          "available unambiguous C integer exports install through existing hook transaction");
    probe::Install(module);
    Check(detour_mock::attaches == 2, "restarting capture does not install duplicate C hooks");
    std::printf("PASS MfgProbe: %u checks\n", g_checks);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL MfgProbe: %s\n", error.what());
    return 1;
  }
}
