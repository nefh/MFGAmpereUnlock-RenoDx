// SPDX-License-Identifier: MIT
// Actual addon scan/restore/unload and UI functions; memory/ImGui are doubles.
#include "../../../src/addons/mfgunlock/provider.hpp"
#include "../../../src/addons/mfgunlock/ngx_hook.hpp"
#include <mutex>
#include <cassert>
#include <iostream>
#include <sstream>
namespace mfgunlock::framecount {
inline std::atomic_bool g_streamline_plugin_seen{false}, g_capacity_advertised{false},g_unknown_ceiling_logged{false};
inline std::atomic_uint g_streamline_max_generated{0};
inline unsigned invalidations=0;
void NotifyDlssgPluginUnloaded(HMODULE) {++invalidations;g_streamline_max_generated=0;g_streamline_plugin_seen=false;}
}
namespace mfgunlock::validatedwarp {
enum class Mode { kValidatedWarp, kOther };
Mode ConfiguredMode(int x) { return x ? Mode::kOther : Mode::kValidatedWarp; }
std::string ModeLabel(Mode,unsigned) {return "TEST_MODE";}
}
namespace mfgunlock::blackwelltemporal {
const char* BoundaryArtifactModeName(unsigned) {return "Balanced";}
}
namespace ImGui {
inline float y=0;inline std::vector<std::string> rows;
float GetCursorPosY(){return y;} float GetTextLineHeightWithSpacing(){return 1;}
void SetCursorPosY(float value){y=value;}
void TextUnformatted(const char* text){rows.emplace_back(text);y+=1;}
void Clear(){y=0;rows.clear();}
}
std::atomic_bool g_ceiling_patched{false}, g_dynamic_stack_validated{false};
std::atomic<HMODULE> g_ceiling_module{nullptr}; std::recursive_mutex g_ceiling_lock;
unsigned char* g_ceiling_site=nullptr;unsigned char g_ceiling_cmov_original=0;
unsigned g_ceiling_compiled=0;mfgunlock::hook::ModuleIdentity g_ceiling_identity{};
SRWLOCK g_provider_maintenance_lock=SRWLOCK_INIT;
std::atomic_int g_validated_warp_mode{0};
struct Warp { mfgunlock::validatedwarp::Mode mode=mfgunlock::validatedwarp::Mode::kValidatedWarp; unsigned target_sm=86; };
std::vector<Warp> g_validated_warp_modules;
std::string g_validated_warp_detail,g_blackwell_temporal_detail;
struct Temporal {unsigned boundary_mode=0,target_sm=86;};
std::vector<Temporal> g_blackwell_temporal_modules,g_midpoint_modules;
std::atomic_bool g_temporal_fix{true},g_midpoint_patched{false};
void RestoreFrameCountCeiling();
#include "addon_runtime_compat_seam.hpp"
struct Image {
  std::vector<unsigned char> bytes=std::vector<unsigned char>(0x3000,0);
  HMODULE Module(){return bytes.data();}
  Image(unsigned ceiling,DWORD stamp){
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;
    std::memcpy(bytes.data(),&dos,sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt.FileHeader.NumberOfSections=1;
    nt.FileHeader.TimeDateStamp=stamp;nt.FileHeader.SizeOfOptionalHeader=sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfHeaders=0x400;nt.OptionalHeader.SizeOfImage=bytes.size();
    std::memcpy(bytes.data()+0x80,&nt,sizeof(nt));
    IMAGE_SECTION_HEADER sec{};sec.VirtualAddress=0x1000;sec.Misc.VirtualSize=0x1000;
    sec.Characteristics=IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ;
    std::memcpy(bytes.data()+0x80+sizeof(nt),&sec,sizeof(sec));
    const unsigned char clamp[]={0xba,static_cast<unsigned char>(ceiling),0,0,0,0x3b,0xca,0x0f,0x42,0xd1};
    std::memcpy(bytes.data()+0x1100,clamp,10);
    provider_mock::regions.push_back({Module(),bytes.size(),MEM_IMAGE,PAGE_EXECUTE_READ});
  }
};
int main(){
  namespace f=mfgunlock::framecount;
  Image local(3,1),ota(5,2);
  ObserveFrameCountCeiling(local.Module());assert(f::g_streamline_max_generated==3);
  PatchFrameCountCeiling(local.Module());assert(local.bytes[0x1109]==0xd2);
  ObserveFrameCountCeiling(ota.Module());
  assert(local.bytes[0x1109]==0xd1&&g_ceiling_module==ota.Module()&&f::g_streamline_max_generated==5);
  const auto writes=provider_mock::protection_calls;
  for(unsigned i=0;i<10000;++i) ObserveFrameCountCeiling(ota.Module());
  assert(provider_mock::protection_calls==writes);
  OnDlssgPluginUnloaded(local.Module());assert(f::invalidations==0&&f::g_streamline_max_generated==5);
  OnDlssgPluginUnloaded(ota.Module());assert(f::invalidations==0); // Old event must not clear a current generation.
  auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(ota.bytes.data()+0x80);
  ++nt->FileHeader.TimeDateStamp;ota.bytes[0x1101]=3;
  ObserveFrameCountCeiling(ota.Module());assert(f::g_streamline_max_generated==3);
  PatchFrameCountCeiling(ota.Module());assert(ota.bytes[0x1109]==0xd2);
  provider_mock::regions.erase(provider_mock::regions.begin()+1);
  OnDlssgPluginUnloaded(ota.Module());assert(f::invalidations==1&&g_ceiling_module==nullptr);
  RestoreFrameCountCeiling(); // Never restore through the disappeared mapping.
  g_validated_warp_modules={{}};
  ImGui::Clear();DrawWarpStatus(true);assert(ImGui::rows==std::vector<std::string>{"Warp: ACTIVE"});
  for(unsigned i=0;i<10000;++i){
    provider_mock::force_lock_busy=(i%2)==0;
    ImGui::Clear();DrawWarpStatus(true);
    assert(ImGui::y==1&&ImGui::rows==std::vector<std::string>{"Warp: ACTIVE"});
  }
  provider_mock::force_lock_busy=false;g_validated_warp_modules.clear();
  ImGui::Clear();DrawWarpStatus(false);assert(ImGui::y==1&&ImGui::rows[0]=="Warp: DISABLED");
  g_validated_warp_detail="no matching optional kernel";
  ImGui::Clear();DrawWarpStatus(true);assert(ImGui::y==1&&ImGui::rows[0]=="Warp: UNAVAILABLE");
  g_blackwell_temporal_modules={{}};
  ImGui::Clear();ProductionDrawIsr(false);assert(ImGui::y==1); // Was two rows for requested-off/applied.
  const auto isr=ImGui::rows;
  provider_mock::force_lock_busy=true;
  ImGui::Clear();ProductionDrawIsr(false);assert(ImGui::y==1&&ImGui::rows==isr);
  provider_mock::force_lock_busy=false;
  ImGui::Clear();ProductionDrawTemporal(true);assert(ImGui::y==1);
  const auto temporal=ImGui::rows;
  provider_mock::force_lock_busy=true;
  ImGui::Clear();ProductionDrawTemporal(true);assert(ImGui::y==1&&ImGui::rows==temporal);
  std::cout<<"runtime_compat_test PASS: current OTA ceiling, unload isolation, 10000 stable scans and 10000 stable UI lock cycles\n";
}
