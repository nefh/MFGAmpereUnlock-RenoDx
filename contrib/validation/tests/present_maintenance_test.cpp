// SPDX-License-Identifier: MIT
// Real production poll and Present decision; OS/loader calls are explicit doubles.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include "../../../src/addons/mfgunlock/ngx_hook.hpp"
#include "../../../src/addons/mfgunlock/diagnostic_bridge.hpp"
#include "../../../src/addons/mfgunlock/presentation_observer.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

struct LARGE_INTEGER { long long QuadPart = 0; };
inline bool QueryPerformanceCounter(LARGE_INTEGER* p) { static long long t = 0; p->QuadPart=++t; return true; }
inline bool QueryPerformanceFrequency(LARGE_INTEGER* p) { p->QuadPart=1000000; return true; }
std::map<std::wstring, HMODULE> modules;
HMODULE GetModuleHandleW(const wchar_t* name) {
  auto it=modules.find(name); return it==modules.end()?nullptr:it->second;
}
std::atomic_bool g_provider_scan_pending{false}, g_streamline_scan_pending{false}, g_ngx_scan_pending{false};
std::atomic_bool g_bootstrap_complete{true};
SRWLOCK g_provider_maintenance_lock = SRWLOCK_INIT;
std::vector<HMODULE> g_dlssg_modules;
unsigned refreshes=0, checks=0;
void TryAttachRuntime() {}
void RefreshRuntime();
#include "addon_maintenance_seam.hpp"
void RefreshRuntime() {
  ++refreshes;
  g_provider_scan_pending=false; g_streamline_scan_pending=false; g_ngx_scan_pending=false;
  PollRuntimeModuleChanges(true);
}
void Check(bool condition, const char* message) {
  ++checks; if (!condition) throw std::runtime_error(message);
}
struct Image {
  std::vector<unsigned char> bytes = std::vector<unsigned char>(0x1000);
  HMODULE module = bytes.data();
  explicit Image(DWORD timestamp) {
    IMAGE_DOS_HEADER dos{}; dos.e_magic=IMAGE_DOS_SIGNATURE; dos.e_lfanew=0x80;
    std::memcpy(bytes.data(), &dos, sizeof(dos));
    IMAGE_NT_HEADERS64 nt{}; nt.Signature=IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64; nt.FileHeader.TimeDateStamp=timestamp;
    nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt.OptionalHeader.SizeOfImage=0x1000;
    std::memcpy(bytes.data()+0x80, &nt, sizeof(nt));
    provider_mock::regions.push_back({module,bytes.size()});
  }
  void Unmap() {std::erase_if(provider_mock::regions,[&](const auto& r){return r.base==module;});}
};
void Tick() { for(unsigned i=0;i<120;++i) ProductionPresentMaintenanceTick(); }
int main() {
  try {
    Image provider(1), interposer(2), common(3), plugin(4), ngx(5), renamed(6);
    modules={{L"nvngx_dlssg.dll",provider.module},{L"sl.interposer.dll",interposer.module},
             {L"sl.common.dll",common.module},{L"sl.dlss_g.dll",plugin.module},{L"_nvngx.dll",ngx.module}};
    g_dlssg_modules={provider.module,renamed.module};
    PollRuntimeModuleChanges(true);
    for(unsigned i=0;i<100;++i) Tick();
    Check(refreshes==0,"stable 12000 Presents must not run full maintenance on a timer");
    modules.erase(L"sl.common.dll"); Tick();
    Check(refreshes==1,"missing common triggers one actual refresh");
    Tick(); Check(refreshes==1,"absence is not an endless retry loop");
    modules[L"sl.common.dll"]=common.module; Tick();
    Check(refreshes==2,"common reload triggers one refresh");
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(plugin.bytes.data()+0x80);
    ++nt->FileHeader.TimeDateStamp; Tick();
    Check(refreshes==3,"same base with changed header identity is noticed");
    renamed.Unmap(); Tick();
    Check(refreshes==4,"renamed provider disappearance is noticed");
    Tick(); Check(refreshes==4,"expired renamed provider does not retrigger");
    g_provider_scan_pending=true; ProductionPresentMaintenanceTick();
    Check(refreshes==5,"real loader event remains immediate, independent of timer");
    provider_mock::force_lock_busy=true;
    modules.erase(L"_nvngx.dll"); Tick();
    Check(refreshes==5,"busy poll lock skips rather than blocking rendering");
    provider_mock::force_lock_busy=false; Tick();
    Check(refreshes==6,"skipped liveness observation retries next tick");
    Check(provider_mock::protection_calls==0 && detour_mock::begins==0 && hook_mock::closed==0,
          "liveness poll does no writes, Detours transactions or module/thread snapshots");
    std::cout<<"present_maintenance_test: "<<checks<<" checks PASS; stable_refreshes=0/12000\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
