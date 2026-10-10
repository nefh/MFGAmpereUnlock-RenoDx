// SPDX-License-Identifier: MIT
// Executes the production Streamline/NGX wrappers against deterministic API
// doubles. This is not a Windows ABI, Detours, GPU, or game test.

#include "streamline_bridge.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <type_traits>

using namespace mfgunlock;

namespace ngx_internal = mfgunlock::ngx::internal;
namespace streamline_internal = mfgunlock::streamline::internal;

namespace mfgunlock::streamline::internal {
struct LegacyPreferences {};
}  // namespace mfgunlock::streamline::internal

namespace {

unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (condition) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}

template <class Function>
FARPROC Proc(Function function) {
  return reinterpret_cast<FARPROC>(function);
}

HMODULE Module(uintptr_t value) {
  return reinterpret_cast<HMODULE>(value);
}

NvPhysicalGpuHandle g_gpu = Module(0x100);
LUID g_luid{10, 20};
unsigned int g_architecture = architecture::kAmpereArchitecture;
unsigned int g_arch_calls = 0;
NvAPI_Status g_arch_status = NVAPI_OK;

NvAPI_Status EnumGpu(NvPhysicalGpuHandle* output, NvU32* count) {
  output[0] = g_gpu;
  *count = 1;
  return NVAPI_OK;
}

NvAPI_Status GpuLuid(NvPhysicalGpuHandle, LUID* output) {
  *output = g_luid;
  return NVAPI_OK;
}

NvAPI_Status GpuArch(NvPhysicalGpuHandle, NV_GPU_ARCH_INFO* output) {
  ++g_arch_calls;
  if (g_arch_status != NVAPI_OK || !output) return g_arch_status;
  output->architecture = g_architecture;
  output->implementation = 2;
  output->revision = 0xa1;
  return NVAPI_OK;
}

unsigned int g_requirement_calls = 0;
unsigned int g_requirement_flags = policy::kAdapterUnsupported;
unsigned int g_requirement_architecture = architecture::kAdaArchitecture;
NVSDK_NGX_Result g_requirement_result = NVSDK_NGX_Result_Success;

NVSDK_NGX_Result RealRequirements(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*,
                                  NVSDK_NGX_FeatureRequirement* output) {
  ++g_requirement_calls;
  if (output && g_requirement_result == NVSDK_NGX_Result_Success) {
    output->FeatureSupported = g_requirement_flags;
    output->MinHWArchitecture = g_requirement_architecture;
    std::strcpy(output->MinOSVersion, "10.0.19041");
  }
  return g_requirement_result;
}

unsigned int g_vulkan_requirement_calls = 0;
NVSDK_NGX_Result RealVulkanRequirements(void*, void*,
                                        const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
                                        NVSDK_NGX_FeatureRequirement* output) {
  ++g_vulkan_requirement_calls;
  return RealRequirements(nullptr, discovery, output);
}

unsigned int g_creates = 0;
unsigned int g_evaluates = 0;
unsigned int g_releases = 0;
unsigned int g_create_architecture = 0;
unsigned int g_evaluate_architecture = 0;
unsigned int g_release_architecture = 0;
NVSDK_NGX_Result g_create_result = NVSDK_NGX_Result_Success;
NVSDK_NGX_Result g_evaluate_result = NVSDK_NGX_Result_Success;
NVSDK_NGX_Result g_release_result = NVSDK_NGX_Result_Success;
NVSDK_NGX_Handle g_handle{99};
bool g_return_handle = true;
std::vector<mfgunlock::diagnostic::EvaluateEvent> g_evaluate_observed;
void ObserveEvaluateEvent(const mfgunlock::diagnostic::EvaluateEvent* event) noexcept {
  if (event) g_evaluate_observed.push_back(*event);
}

NVSDK_NGX_Result RealCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                            NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** output) {
  ++g_creates;
  NV_GPU_ARCH_INFO info{};
  info.version = NV_GPU_ARCH_INFO_VER;
  if (ngx_internal::HookedGetArchInfo(g_gpu, &info) == NVAPI_OK)
    g_create_architecture = info.architecture;
  if (output) *output = g_return_handle ? &g_handle : nullptr;
  return g_create_result;
}

NVSDK_NGX_Result RealEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                              const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  ++g_evaluates;
  NV_GPU_ARCH_INFO info{};
  info.version = NV_GPU_ARCH_INFO_VER;
  if (ngx_internal::HookedGetArchInfo(g_gpu, &info) == NVAPI_OK)
    g_evaluate_architecture = info.architecture;
  return g_evaluate_result;
}

NVSDK_NGX_Result RealRelease(NVSDK_NGX_Handle*) {
  ++g_releases;
  NV_GPU_ARCH_INFO info{};
  info.version = NV_GPU_ARCH_INFO_VER;
  if (ngx_internal::HookedGetArchInfo(g_gpu, &info) == NVAPI_OK)
    g_release_architecture = info.architecture;
  return g_release_result;
}

NVSDK_NGX_Parameter g_parameters;
unsigned int g_parameter_calls = 0;
NVSDK_NGX_Result g_parameter_result = NVSDK_NGX_Result_Success;

NVSDK_NGX_Result RealParameters(NVSDK_NGX_Parameter** output) {
  ++g_parameter_calls;
  if (output) *output = g_parameter_result == NVSDK_NGX_Result_Success ? &g_parameters : nullptr;
  return g_parameter_result;
}

unsigned int g_provider_maintenance_calls = 0;
void PrepareProviders() {
  ++g_provider_maintenance_calls;
}

unsigned int CapabilityLimit() {
  return 3;
}

unsigned int g_structural_ceiling = 0;
unsigned int StreamlineCeiling() {
  return g_structural_ceiling;
}

sl::Result g_sl_result = sl::Result::eErrorNoSupportedAdapterFound;
unsigned int g_init_calls = 0;
const sl::Preferences* g_original_preferences = nullptr;

sl::Result RealInit(const sl::Preferences& preferences, uint64_t) {
  ++g_init_calls;
  Check(&preferences == g_original_preferences, "same preferences reference");
  return g_sl_result;
}

unsigned int g_support_calls = 0;
unsigned int g_support_architecture = 0;
sl::Result g_forced_support_result = sl::Result::eOk;
bool g_force_support_result = false;

sl::Result g_feature_requirements_result = sl::Result::eOk;
unsigned int g_feature_requirements_calls = 0;
sl::Result RealFeatureRequirements(sl::Feature feature, sl::FeatureRequirements&) {
  ++g_feature_requirements_calls;
  return feature == sl::kFeatureDLSS_G ? g_feature_requirements_result
                                       : sl::Result::eErrorFeatureMissing;
}

bool g_feature_loaded_value = true;
sl::Result g_feature_loaded_result = sl::Result::eOk;
unsigned int g_feature_loaded_calls = 0;
sl::Result RealFeatureLoaded(sl::Feature feature, bool& loaded) {
  ++g_feature_loaded_calls;
  loaded = g_feature_loaded_value;
  return feature == sl::kFeatureDLSS_G ? g_feature_loaded_result
                                       : sl::Result::eErrorFeatureMissing;
}

sl::Result g_set_device_result = sl::Result::eOk;
unsigned int g_set_device_calls = 0;
sl::Result RealSetD3DDevice(void*) {
  ++g_set_device_calls;
  return g_set_device_result;
}

sl::Result RealSupport(sl::Feature feature, const sl::AdapterInfo&) {
  ++g_support_calls;
  if (feature != sl::kFeatureDLSS_G) return sl::Result::eErrorFeatureMissing;
  if (g_force_support_result) return g_forced_support_result;
  NV_GPU_ARCH_INFO info{};
  info.version = NV_GPU_ARCH_INFO_VER;
  if (ngx_internal::HookedGetArchInfo(g_gpu, &info) != NVAPI_OK)
    return sl::Result::eErrorAdapterNotSupported;
  g_support_architecture = info.architecture;
  return info.architecture == architecture::kAdaArchitecture
      ? sl::Result::eOk
      : sl::Result::eErrorAdapterNotSupported;
}

bool g_legacy_discovery_result = false;
unsigned int g_legacy_discovery_calls = 0;
sl::Feature g_legacy_discovery_feature = 0;
bool DiscoverySupport(sl::Feature feature, uint32_t* mask) {
  ++g_legacy_discovery_calls;
  g_legacy_discovery_feature = feature;
  if (g_legacy_discovery_result && mask) *mask = 5;
  SetLastError(1234);
  return g_legacy_discovery_result;
}
bool g_set_enabled_argument = false;
bool DiscoveryEnabled(sl::Feature feature) { return DiscoverySupport(feature, nullptr); }
bool DiscoverySetEnabled(sl::Feature feature, bool enabled) {
  g_set_enabled_argument = enabled;
  return DiscoverySupport(feature, nullptr);
}
bool DiscoverySettings(sl::Feature feature, const void*, void*) { return DiscoverySupport(feature, nullptr); }
unsigned int g_passive_lookups = 0;
unsigned int g_passive_loads = 0;
unsigned int g_passive_starts = 0;
const char kPassiveJson[] = "{\"supportedAdapters\":0,\"name\":\"sl.dlss\"}";
bool PassiveLoad(sl::param::IParameters*, const char*, const char** output) {
  ++g_passive_loads;
  if (output) *output = kPassiveJson;
  SetLastError(4567);
  return g_legacy_discovery_result;
}
static_assert(std::is_same_v<decltype(&PassiveLoad), streamline_internal::LoadFn>);
bool PassiveStartup(const char*, void*) {
  ++g_passive_starts;
  SetLastError(5678);
  return g_legacy_discovery_result;
}
void* PassiveGateway(const char* name) {
  ++g_passive_lookups;
  SetLastError(6789);
  if (!std::strcmp(name, "slOnPluginLoad")) return reinterpret_cast<void*>(PassiveLoad);
  if (!std::strcmp(name, "slOnPluginStartup")) return reinterpret_cast<void*>(PassiveStartup);
  return nullptr;
}
unsigned int g_legacy_init_calls = 0;
int g_legacy_application_id = 0;
bool g_legacy_result = true;

bool RealLegacyInit(const streamline_internal::LegacyPreferences&, int application_id) {
  ++g_legacy_init_calls;
  g_legacy_application_id = application_id;
  return g_legacy_result;
}

bool g_plugin_result = true;
unsigned int g_loads = 0;
unsigned int g_startups = 0;
unsigned int g_load_architecture = 0;
unsigned int g_startup_architecture = 0;
const char* g_plugin_json = "{\"supportedAdapters\":0}";
unsigned int g_plugin_bound_calls = 0;
HMODULE g_bound_plugin = nullptr;
unsigned int g_plugin_unloaded_calls = 0;
HMODULE g_unloaded_plugin = nullptr;

void ObservePlugin(HMODULE module) {
  ++g_plugin_bound_calls;
  g_bound_plugin = module;
}

void ObservePluginUnload(HMODULE module) {
  ++g_plugin_unloaded_calls;
  g_unloaded_plugin = module;
}

bool RealLoad(sl::param::IParameters*, const char*, const char** output) {
  ++g_loads;
  NV_GPU_ARCH_INFO info{};
  info.version = NV_GPU_ARCH_INFO_VER;
  if (ngx_internal::HookedGetArchInfo(g_gpu, &info) == NVAPI_OK)
    g_load_architecture = info.architecture;
  if (output) *output = g_plugin_json;
  return g_plugin_result;
}

bool RealStartup(const char*, void*) {
  ++g_startups;
  NV_GPU_ARCH_INFO info{};
  info.version = NV_GPU_ARCH_INFO_VER;
  if (ngx_internal::HookedGetArchInfo(g_gpu, &info) == NVAPI_OK)
    g_startup_architecture = info.architecture;
  return g_plugin_result;
}

void Unrelated() {}

unsigned int g_gateway_lookups = 0;
void* RealGateway(const char* name) {
  ++g_gateway_lookups;
  if (!name) return nullptr;
  if (std::strcmp(name, "slOnPluginLoad") == 0) return reinterpret_cast<void*>(&RealLoad);
  if (std::strcmp(name, "slOnPluginStartup") == 0) return reinterpret_cast<void*>(&RealStartup);
  if (std::strcmp(name, "slDLSSGSetOptions") == 0 ||
      std::strcmp(name, "slDLSSGGetState") == 0)
    return reinterpret_cast<void*>(&Unrelated);
  return nullptr;
}

void SetVersion(HMODULE module, unsigned int major, unsigned int minor) {
  auto& resource = mock::resources[module];
  resource.assign(40 + sizeof(VS_FIXEDFILEINFO), 0);
  const uint16_t length = static_cast<uint16_t>(resource.size());
  const uint16_t value_length = sizeof(VS_FIXEDFILEINFO);
  std::memcpy(resource.data(), &length, sizeof(length));
  std::memcpy(resource.data() + 2, &value_length, sizeof(value_length));
  constexpr char16_t kVersionKey[] = u"VS_VERSION_INFO";
  std::memcpy(resource.data() + 6, kVersionKey, sizeof(kVersionKey));
  VS_FIXEDFILEINFO version{};
  version.dwSignature = 0xfeef04bd;
  version.dwFileVersionMS = (major << 16) | minor;
  std::memcpy(resource.data() + 40, &version, sizeof(version));
}

void ResetParameters(int available, int maximum, int needs_driver,
                     unsigned int init_result = NVSDK_NGX_Result_Success) {
  g_parameters.values.clear();
  g_parameters.writes = 0;
  g_parameters.writable = true;
  g_parameters.values["FrameGeneration.Available"] = static_cast<uint32_t>(available);
  g_parameters.values["DLSSG.MultiFrameCountMax"] = static_cast<uint32_t>(maximum);
  g_parameters.values["FrameGeneration.NeedsUpdatedDriver"] = static_cast<uint32_t>(needs_driver);
  g_parameters.values["FrameGeneration.FeatureInitResult"] = init_result;
}

}  // namespace


std::vector<unsigned char> VersionNode(const std::u16string& key, uint16_t type,
    const std::vector<unsigned char>& value,
    const std::vector<std::vector<unsigned char>>& children = {}) {
  std::vector<unsigned char> result(6, 0);
  for (const auto ch : key) { result.push_back(static_cast<unsigned char>(ch)); result.push_back(ch >> 8); }
  result.insert(result.end(), 2, 0);
  while (result.size() % 4) result.push_back(0);
  result.insert(result.end(), value.begin(), value.end());
  for (const auto& child : children) {
    while (result.size() % 4) result.push_back(0);
    result.insert(result.end(), child.begin(), child.end());
  }
  const uint16_t length = static_cast<uint16_t>(result.size());
  const uint16_t count = static_cast<uint16_t>(type == 1 ? value.size() / 2 : value.size());
  std::memcpy(result.data(), &length, 2);
  std::memcpy(result.data() + 2, &count, 2);
  std::memcpy(result.data() + 4, &type, 2);
  return result;
}
std::vector<unsigned char> VersionText(const std::u16string& text) {
  std::vector<unsigned char> value;
  for (auto ch : text) { value.push_back(static_cast<unsigned char>(ch)); value.push_back(ch >> 8); }
  value.insert(value.end(), 2, 0);
  return value;
}
void DisplayVersionTests() {
  using namespace streamline_internal;
  const auto module = Module(0x123456);
  SetVersion(module, 1, 0);
  const auto fixed_blob = mock::resources[module];
  const std::vector<unsigned char> fixed(fixed_blob.begin() + 40, fixed_blob.end());
  const auto make = [&](const std::u16string& value) {
    return VersionNode(u"VS_VERSION_INFO", 0, fixed,
        {VersionNode(u"StringFileInfo", 1, {},
          {VersionNode(u"040904b0", 1, {}, {VersionNode(u"FileVersion", 1, VersionText(value))})})});
  };
  mock::resources[module] = make(u"1.5.4.0");
  Check(ReadModuleVersion(module).minor == 0, "fixed version retained as raw metadata");
  const auto display = ReadModuleDisplayVersion(module);
  Check(display.major == 1 && display.minor == 5 && display.build == 4 && display.revision == 0,
        "StringFileInfo preferred for diagnostic version");
  for (const auto* value : {u"1, 5, 4, 0", u" 1.5.4.0 ", u"1.5.4.65535"}) {
    mock::resources[module] = make(value);
    Check(ReadModuleDisplayVersion(module).minor == 5, "comma/space version syntax");
  }
  for (const auto* value : {u"1.5.4.65536", u"1.5.4.0oops", u"1..4.0", u"1.5.4", u"garbage"}) {
    mock::resources[module] = make(value);
    Check(ReadModuleDisplayVersion(module).minor == 0, "malformed display falls back to fixed");
  }
  auto valid = make(u"1.5.4.0");
  for (size_t bytes = 0; bytes < valid.size(); ++bytes) {
    mock::resources[module].assign(valid.begin(), valid.begin() + bytes);
    Check(ReadModuleDisplayVersion(module).minor != 5, "truncated resource never exposes string version");
  }
  mock::resources[module] = VersionNode(u"VS_VERSION_INFO", 0, fixed,
      {VersionNode(u"StringFileInfo", 1, {}, {VersionNode(u"040904b0", 1, {},
        {VersionNode(u"FileVersion", 1, VersionText(u"1.5.4.0")),
         VersionNode(u"FileVersion", 1, VersionText(u"99.1.0.0"))})})});
  Check(ReadModuleDisplayVersion(module).minor == 0, "ambiguous display versions fall back");
  mock::resources[module] = fixed_blob;
  Check(ReadModuleDisplayVersion(module).major == 1, "fixed-only resource fallback");
  mock::resources.erase(module);
  Check(ReadModuleDisplayVersion(module).major == 0, "absent version stays unknown");
  // All offsets in the parser are relative; arbitrary input alignment is valid.
  valid.insert(valid.begin(), 0);
  bool found = false;
  unsigned budget = 256;
  ModuleVersion unaligned;
  Check(FindDisplayVersion(std::span<const unsigned char>(valid).subspan(1), 0,
      valid.size() - 1, 0, false, budget, found, unaligned) && found && unaligned.minor == 5,
      "unaligned resource byte buffer");
}
void TestDiscoveryPassThrough() {
  const auto state = streamline_internal::g_lifecycle_state.load();
  const auto maintenance = g_provider_maintenance_calls;
  const auto binds = g_plugin_bound_calls;
  const auto unloads = g_plugin_unloaded_calls;
  const auto scope = ngx_internal::g_arch_scope;
  mock::readable_regions[reinterpret_cast<uintptr_t>(kPassiveJson)] = sizeof(kPassiveJson);
  Check(streamline_internal::DiscoveryJson(kPassiveJson) == kPassiveJson, "bounded JSON exact read");
  Check(streamline_internal::DiscoveryJson(nullptr) == "NULL", "null JSON is not dereferenced");
  Check(streamline_internal::DiscoveryJson(reinterpret_cast<const char*>(1)) == "<UNREADABLE>",
        "unmapped JSON not dereferenced");
  std::string large(5000, 'a');
  mock::readable_regions[reinterpret_cast<uintptr_t>(large.data())] = large.size();
  Check(streamline_internal::DiscoveryJson(large.data()) == std::string(4096, 'a') + "<TRUNCATED>",
        "diagnostic JSON is capped");
  mock::readable_regions.erase(reinterpret_cast<uintptr_t>(large.data()));

  auto sr = Module(0x5158);
  mock::modules[L"sl.dlss.dll"] = sr;
  mock::paths[sr] = L"X:\\fixture\\sl.dlss.dll";
  mock::exports[{sr, "slGetPluginFunction"}] = Proc(PassiveGateway);
  mock::allocation[reinterpret_cast<const void*>(Proc(PassiveGateway))] = sr;
  const auto before = g_passive_lookups;
  Check(streamline_internal::BindPlugin(sr, Proc(PassiveGateway)) == Proc(PassiveGateway),
        "SR observer returns native gateway");
  Check(g_passive_lookups == before && g_plugin_bound_calls == binds,
        "passive bind neither probes gateway nor publishes a FG bind");
  auto& runtime = streamline_internal::g_plugins[1];
  Check(runtime.module == sr && runtime.role == streamline_internal::PluginRole::kSr,
        "SR observer occupies the existing role-tagged registry");
  auto gateway = reinterpret_cast<streamline_internal::GatewayFn>(runtime.gateway_hook.replacement.load());
  Check(gateway && gateway("slOnPluginLoad") == reinterpret_cast<void*>(PassiveLoad) && GetLastError() == 6789,
        "SR load callback lookup preserves pointer and LastError");
  Check(gateway("slOnPluginStartup") == reinterpret_cast<void*>(PassiveStartup) && GetLastError() == 6789,
        "SR startup callback lookup preserves pointer and LastError");
  auto load = reinterpret_cast<streamline_internal::LoadFn>(runtime.load_hook.replacement.load());
  auto startup = reinterpret_cast<streamline_internal::StartupFn>(runtime.startup_hook.replacement.load());
  const char* output = nullptr;
  g_legacy_discovery_result = true;
  Check(load && load(nullptr, kPassiveJson, &output) && output == kPassiveJson && g_passive_loads == 1 && GetLastError() == 4567,
        "SR onLoad original result, output pointer, call count and LastError");
  Check(startup && startup(kPassiveJson, nullptr) && g_passive_starts == 1 && GetLastError() == 5678,
        "SR startup original result, count and LastError");
  g_legacy_discovery_result = false;
  Check(!load(nullptr, kPassiveJson, &output) && g_passive_loads == 2 && GetLastError() == 4567,
        "SR load rejection is not changed to success");
  Check(!startup(kPassiveJson, nullptr) && g_passive_starts == 2 && GetLastError() == 5678,
        "SR startup rejection is preserved");
  const auto repeat_count = g_passive_lookups;
  Check(streamline_internal::BindPlugin(sr, Proc(PassiveGateway)) == Proc(PassiveGateway) &&
        g_passive_lookups == repeat_count, "passive repeated attach is idempotent");
  Check(streamline_internal::g_lifecycle_state.load() == state &&
        g_provider_maintenance_calls == maintenance && ngx_internal::g_arch_scope == scope,
        "SR diagnostics do not mutate FG lifecycle, prepare providers or change spoof scope");
  mock::module_current[runtime.gateway_hook.identity.module] = false;
  streamline_internal::RefreshPluginLifetimes();
  Check(!runtime.module && g_plugin_unloaded_calls == unloads &&
        streamline_internal::g_lifecycle_state.load() == state,
        "SR unload cannot publish FG unload");
  mock::module_current[reinterpret_cast<HMODULE>(Proc(PassiveGateway))] = true;
  mock::install_ok = false;
  streamline_internal::BindPlugin(sr, Proc(PassiveGateway));
  Check(!runtime.module && runtime.role == streamline_internal::PluginRole::kFg,
        "failed passive hook cannot leave an SR role in a slot later used by FG");
  mock::install_ok = true;
  mock::readable_regions.clear();
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--version-resource") == 0) {
    std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > 65535) return 2;
    std::vector<unsigned char> bytes(static_cast<size_t>(input.tellg()));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return 2;
    const auto module = Module(0x123456);
    mock::resources[module] = bytes;
    const auto fixed = streamline_internal::ReadModuleVersion(module);
    const auto display = streamline_internal::ReadModuleDisplayVersion(module);
    std::cout << "fixed=" << fixed.major << '.' << fixed.minor << '.' << fixed.build << '.' << fixed.revision
              << " display=" << display.major << '.' << display.minor << '.' << display.build << '.' << display.revision << '\n';
    return 0;
  }
  DisplayVersionTests();
  g_enabled = true;
  architecture::Configure(Architecture::kAmpere);
  mfgunlock::provider::g_test_prepared_count = 1;

  auto& nvapi = ngx_internal::GetNvapi();
  nvapi.enumerate = EnumGpu;
  nvapi.adapter_id = GpuLuid;
  nvapi.architecture = GpuArch;
  nvapi.ready = true;

  IDXGIAdapter adapter{};
  adapter.desc.VendorId = architecture::kNvidiaVendorId;
  adapter.desc.AdapterLuid = g_luid;
  Check(ngx_internal::MatchAdapter(&adapter) == g_gpu, "physical LUID match");
  Check(g_arch_calls == 0, "explicit profile does not query architecture");

  NV_GPU_ARCH_INFO arch_info{};
  arch_info.version = NV_GPU_ARCH_INFO_VER;
  ngx_internal::HookedGetArchInfo(g_gpu, &arch_info);
  Check(arch_info.architecture == architecture::kAmpereArchitecture, "no NVAPI spoof outside scope");
  {
    ngx_internal::ScopedArchQuery scope(g_gpu);
    ngx_internal::HookedGetArchInfo(g_gpu, &arch_info);
    Check(arch_info.architecture == architecture::kAdaArchitecture && arch_info.implementation == 2,
          "scoped architecture exposure");
  }

  auto& runtime = ngx_internal::g_slots[0];
  runtime.entry_requirements = RealRequirements;
  runtime.entry_vulkan_requirements = RealVulkanRequirements;
  runtime.entry_create = RealCreate;
  runtime.entry_evaluate = RealEvaluate;
  runtime.entry_release = RealRelease;
  runtime.parameters[0] = RealParameters;
  runtime.parameters[2] = RealParameters;
  ngx_internal::g_bound_gpu = g_gpu;
  mfgunlock::ngx::g_prepare_loaded_providers = PrepareProviders;
  mfgunlock::ngx::g_capability_limit = CapabilityLimit;
  mfgunlock::ngx::g_streamline_ceiling = StreamlineCeiling;

  NVSDK_NGX_FeatureDiscoveryInfo discovery{};
  discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
  NVSDK_NGX_FeatureRequirement requirements{};

  Check(ngx_internal::Requirements<0>(&adapter, &discovery, &requirements) == NVSDK_NGX_Result_Success,
        "D3D12 requirements result preserved");
  Check(requirements.FeatureSupported == 0 &&
            requirements.MinHWArchitecture == architecture::kAmpereArchitecture &&
            std::strcmp(requirements.MinOSVersion, "10.0.19041") == 0,
        "D3D12 requirements adjust architecture only");

  discovery.FeatureID = static_cast<NVSDK_NGX_Feature>(1);
  ngx_internal::Requirements<0>(&adapter, &discovery, &requirements);
  Check(requirements.FeatureSupported == policy::kAdapterUnsupported &&
            requirements.MinHWArchitecture == architecture::kAdaArchitecture,
        "non-FG requirements remain native");
  discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;

  g_requirement_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  requirements.MinHWArchitecture = 123;
  Check(ngx_internal::Requirements<0>(&adapter, &discovery, &requirements) == g_requirement_result &&
            requirements.MinHWArchitecture == 123,
        "requirements failure output not rewritten");
  g_requirement_result = NVSDK_NGX_Result_Success;

  const auto vulkan_before = g_vulkan_requirement_calls;
  ngx_internal::VulkanRequirements<0>(nullptr, nullptr, &discovery, &requirements);
  Check(g_vulkan_requirement_calls == vulkan_before + 1 &&
            requirements.FeatureSupported == g_requirement_flags &&
            requirements.MinHWArchitecture == g_requirement_architecture,
        "Vulkan cannot inherit a DXGI binding without its own physical identity");

  ResetParameters(0, 1, 0, NVSDK_NGX_Result_FAIL_FeatureNotSupported);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  int value = 0;
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 1,
        "eligible provider exposes frame generation");
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 3,
        "capability maximum raised to verified limit");

  ResetParameters(0, 1, 0, NVSDK_NGX_Result_FAIL_FeatureNotSupported);
  ngx_internal::ApplyCapabilities(&g_parameters, 0);
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 1,
        "base FG is admitted after preparation even when optional MFG count is unavailable");
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 1,
        "admitting base FG does not invent a higher frame count");
  mfgunlock::provider::g_test_prepared_count = 0;
  ResetParameters(0, 1, 0, NVSDK_NGX_Result_FAIL_FeatureNotSupported);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 0,
        "requested count alone cannot claim successful provider preparation");
  mfgunlock::provider::g_test_prepared_count = 1;

  ResetParameters(0, 1, 1);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 0,
        "explicit driver requirement is preserved");

  ResetParameters(1, 5, 0);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 5,
        "larger native capability is never reduced");

  ResetParameters(1, 5, 0);
  ngx_internal::ApplyCapabilities(&g_parameters, 5, 3);
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 3,
        "known Streamline ceiling caps NGX capability advertising");

  ResetParameters(1, 1, 0);
  ngx_internal::ApplyCapabilities(&g_parameters, 5, 0);
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 5,
        "direct NGX capability path has no Streamline ceiling");

  architecture::Configure(Architecture::kAda);
  ResetParameters(1, 1, 0);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 3,
        "native Ada capability bridge raises the MFG count without provider retargeting");

  ResetParameters(0, 1, 0, NVSDK_NGX_Result_FAIL_FeatureNotSupported);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 0,
        "native Ada capability bridge never overrides native FG admission");

  ResetParameters(1, 1, 0);
  g_provider_maintenance_calls = 0;
  NVSDK_NGX_Parameter* ada_output_parameters = nullptr;
  Check(ngx_internal::Capabilities<0, 0>(&ada_output_parameters) == NVSDK_NGX_Result_Success &&
            ada_output_parameters == &g_parameters && g_provider_maintenance_calls == 2,
        "native Ada executes the NGX capability bridge");
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 3,
        "native Ada capability wrapper exposes the verified MFG count");
  architecture::Configure(Architecture::kAmpere);

  ResetParameters(0, 1, 0, NVSDK_NGX_Result_FAIL_FeatureNotSupported);
  g_provider_maintenance_calls = 0;
  NVSDK_NGX_Parameter* output_parameters = nullptr;
  Check(ngx_internal::Capabilities<0, 0>(&output_parameters) == NVSDK_NGX_Result_Success &&
            output_parameters == &g_parameters && g_provider_maintenance_calls == 2,
        "D3D12 capability path runs preflight and postflight");
  Check(mfgunlock::ngx::g_status.capabilities_seen.load() &&
            !mfgunlock::ngx::g_status.vulkan_capabilities.load(),
        "D3D12 capability status recorded");

  ResetParameters(1, 1, 0);
  Check(ngx_internal::Capabilities<0, 2>(&output_parameters) == NVSDK_NGX_Result_Success &&
            mfgunlock::ngx::g_status.vulkan_capabilities.load(),
        "Vulkan capability path uses the shared policy");

  NVSDK_NGX_Handle* output_handle = nullptr;
  g_create_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  mfgunlock::ngx::g_status.feature_created = false;
  Check(ngx_internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &output_handle) ==
            g_create_result && !mfgunlock::ngx::g_status.feature_created.load(),
        "failed Create remains failed");

  g_create_result = NVSDK_NGX_Result_Success;
  g_return_handle = true;
  ngx_internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &output_handle);
  Check(output_handle == &g_handle && ngx_internal::IsTracked(runtime, &g_handle) &&
            mfgunlock::ngx::g_status.feature_created.load(),
        "successful FG Create tracks handle");
  Check(g_create_architecture == architecture::kAmpereArchitecture,
        "Ampere NGX Create keeps native execution architecture");

  NVSDK_NGX_Handle unrelated{};
  mfgunlock::ngx::g_status.feature_active = false;
  ngx_internal::Evaluate<0>(nullptr, &unrelated, nullptr, nullptr);
  Check(!mfgunlock::ngx::g_status.feature_active.load(), "unrelated Evaluate not marked active");
  ID3D12Resource interpolated{};
  interpolated.desc.Width = 1920;
  interpolated.desc.Height = 1080;
  interpolated.desc.Format = 28;
  NVSDK_NGX_Parameter evaluate_parameters;
  evaluate_parameters.values["DLSSG.MultiFrameCount"] = 3;
  evaluate_parameters.values["DLSSG.MultiFrameIndex"] = 2;
  evaluate_parameters.values["DLSSG.Reset"] = 0;
  evaluate_parameters.values["DLSSG.AutomodeOverrideReset"] = 0;
  evaluate_parameters.values["DLSSG.BackbufferFrameID"] = 41;
  evaluate_parameters.resources["DLSSG.OutputInterpolated"] = &interpolated;
  g_evaluate_observed.clear();
  mfgunlock::diagnostic::g_evaluate_callback.store(ObserveEvaluateEvent, std::memory_order_release);
  ngx_internal::Evaluate<0>(nullptr, &g_handle, &evaluate_parameters, nullptr);
  mfgunlock::diagnostic::g_evaluate_callback.store(nullptr, std::memory_order_release);
  Check(mfgunlock::ngx::g_status.feature_active.load(), "tracked FG Evaluate marked active");
  Check(g_evaluate_architecture == architecture::kAmpereArchitecture,
        "Ampere NGX Evaluate keeps native execution architecture");
  Check(g_evaluate_observed.size() == 2 &&
            g_evaluate_observed[0].phase == mfgunlock::diagnostic::EvaluatePhase::kBegin &&
            g_evaluate_observed[1].phase == mfgunlock::diagnostic::EvaluatePhase::kEnd,
        "tracked Evaluate publishes begin and end only when observer attached");
  Check(g_evaluate_observed[0].evaluation_id != 0 &&
            g_evaluate_observed[0].evaluation_id == g_evaluate_observed[1].evaluation_id &&
            g_evaluate_observed[0].generated_count_known && g_evaluate_observed[0].generated_count == 3 &&
            g_evaluate_observed[0].generated_index_known && g_evaluate_observed[0].generated_index == 2 &&
            g_evaluate_observed[0].reset_known && g_evaluate_observed[0].reset == 0 &&
            g_evaluate_observed[0].automode_reset_known &&
            g_evaluate_observed[0].automode_reset == 0 &&
            g_evaluate_observed[0].backbuffer_frame_id_known &&
            g_evaluate_observed[0].backbuffer_frame_id == 41,
        "Evaluate observer captures exact DLSS-G count, index, reset state and backbuffer frame id");
  const auto& output_snapshot = g_evaluate_observed[0].resources[
      static_cast<size_t>(mfgunlock::diagnostic::ResourceKey::kOutputInterpolated)];
  Check(output_snapshot.known && output_snapshot.object == reinterpret_cast<uint64_t>(&interpolated) &&
            output_snapshot.width == 1920 && output_snapshot.height == 1080 && output_snapshot.format == 28,
        "Evaluate observer snapshots interpolated output metadata");
  Check(g_evaluate_observed[1].result == static_cast<uint32_t>(g_evaluate_result),
        "Evaluate observer end preserves real result");

  g_release_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  ngx_internal::Release<0>(&g_handle);
  Check(ngx_internal::IsTracked(runtime, &g_handle), "failed Release keeps handle");
  g_release_result = NVSDK_NGX_Result_Success;
  ngx_internal::Release<0>(&g_handle);
  Check(!ngx_internal::IsTracked(runtime, &g_handle), "successful Release removes handle");
  Check(g_release_architecture == architecture::kAmpereArchitecture,
        "Ampere NGX Release keeps native execution architecture");

  output_handle = nullptr;
  ngx_internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &output_handle);
  const auto lifecycle_before_device_reset = ngx_internal::LifecycleSnapshot();
  Check(output_handle == &g_handle && lifecycle_before_device_reset.handles == 1,
        "device recreation fixture starts with one tracked FG handle");
  Check(mfgunlock::ngx::ResetFeatureTrackingForDeviceRecreation(),
        "quiescent device recreation invalidates NGX handle ownership");
  const auto lifecycle_after_device_reset = ngx_internal::LifecycleSnapshot();
  Check(!ngx_internal::IsTracked(runtime, &g_handle) &&
            lifecycle_after_device_reset.handles == 0 &&
            lifecycle_after_device_reset.epoch == lifecycle_before_device_reset.epoch + 1 &&
            !mfgunlock::ngx::g_status.feature_created.load() &&
            !mfgunlock::ngx::g_status.feature_active.load(),
        "device recreation clears stale NGX feature state and advances epoch");
  output_handle = nullptr;
  ngx_internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &output_handle);
  Check(output_handle == &g_handle && ngx_internal::IsTracked(runtime, &g_handle),
        "FG feature can be recreated after device-owned state invalidation");
  ngx_internal::Release<0>(&g_handle);

  architecture::Configure(Architecture::kTuring);
  g_architecture = architecture::kTuringArchitecture;
  output_handle = nullptr;
  g_create_architecture = 0;
  g_evaluate_architecture = 0;
  g_release_architecture = 0;
  g_create_result = NVSDK_NGX_Result_Success;
  g_evaluate_result = NVSDK_NGX_Result_Success;
  g_release_result = NVSDK_NGX_Result_Success;
  ngx_internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &output_handle);
  Check(output_handle == &g_handle && g_create_architecture == architecture::kTuringArchitecture,
        "Turing NGX Create keeps native execution architecture");
  ngx_internal::Evaluate<0>(nullptr, &g_handle, nullptr, nullptr);
  Check(g_evaluate_architecture == architecture::kTuringArchitecture,
        "Turing NGX Evaluate keeps native execution architecture");
  ngx_internal::Release<0>(&g_handle);
  Check(g_release_architecture == architecture::kTuringArchitecture,
        "Turing NGX Release keeps native execution architecture");
  architecture::Configure(Architecture::kAmpere);
  g_architecture = architecture::kAmpereArchitecture;

  sl::Preferences preferences{};
  preferences.flags = static_cast<sl::PreferenceFlags>(0x69);
  const sl::Feature unrelated_features[] = {99};
  preferences.featuresToLoad = unrelated_features;
  preferences.numFeaturesToLoad = 1;
  g_original_preferences = &preferences;
  streamline_internal::g_lifecycle_state = streamline_internal::LifecycleState::kNotSeen;
  Check(mfgunlock::streamline::OnInit(preferences, sl::kSDKVersion, RealInit) == g_sl_result &&
            streamline_internal::g_feature_requested.load() == 0 &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kNotSeen,
        "modern slInit distinguishes feature not requested from unknown");

  preferences.featuresToLoad = nullptr;
  preferences.numFeaturesToLoad = 1;
  streamline_internal::g_lifecycle_state = streamline_internal::LifecycleState::kNotSeen;
  Check(mfgunlock::streamline::OnInit(preferences, sl::kSDKVersion, RealInit) == g_sl_result &&
            streamline_internal::g_feature_requested.load() == -1 &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kUnknown,
        "malformed feature list remains UNKNOWN");

  const sl::Feature requested_features[] = {99, sl::kFeatureDLSS_G};
  preferences.featuresToLoad = requested_features;
  preferences.numFeaturesToLoad = 2;
  g_original_preferences = &preferences;
  const auto original_flags = preferences.flags;
  Check(mfgunlock::streamline::OnInit(preferences, sl::kSDKVersion, RealInit) == g_sl_result,
        "modern slInit result preserved");
  Check(preferences.flags == original_flags, "modern slInit preferences untouched");
  Check(streamline_internal::g_feature_requested.load() == 1 &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kRequested,
        "modern slInit observes DLSS-G in featuresToLoad");

  const auto interposer = Module(0x5151);
  mock::modules[L"sl.common.dll"] = Module(0x5152);
  mock::modules[L"sl.interposer.dll"] = interposer;
  mock::paths[interposer] = L"X:\\fixture\\sl.interposer.dll";
  SetVersion(interposer, 1, 5);
  mock::exports[{interposer, "slInit"}] = Proc(RealLegacyInit);
  for (const char* name : {"slShutdown", "slSetTag", "slEvaluateFeature",
                           "slGetFeatureSettings", "slSetFeatureConstants"})
    mock::exports[{interposer, name}] = Proc(Unrelated);
  mock::exports[{interposer, "slSetFeatureEnabled"}] = Proc(DiscoverySetEnabled);
  mock::allocation[reinterpret_cast<const void*>(Proc(DiscoverySetEnabled))] = interposer;
  mock::exports[{interposer, "slIsFeatureSupported"}] = Proc(DiscoverySupport);
  mock::exports[{interposer, "slIsFeatureEnabled"}] = Proc(DiscoveryEnabled);
  mock::exports[{interposer, "slGetFeatureSettings"}] = Proc(DiscoverySettings);
  for (auto function : {Proc(DiscoverySupport), Proc(DiscoveryEnabled), Proc(DiscoverySettings)})
    mock::allocation[reinterpret_cast<const void*>(function)] = interposer;
  mock::allocation[reinterpret_cast<const void*>(Proc(RealLegacyInit))] = interposer;
  mock::allocation[reinterpret_cast<const void*>(Proc(Unrelated))] = interposer;
  Check(streamline_internal::GetInterposerAbi(interposer) == streamline_internal::InterposerAbi::kLegacy,
        "Streamline 1.x ABI detected");
  Check(!mfgunlock::streamline::CanHookInit(interposer), "legacy slInit excluded from modern ABI hook");
  streamline_internal::InstallLegacyInitHook();
  Check(streamline_internal::g_legacy_init_hooked.load() && streamline_internal::g_real_legacy_init == RealLegacyInit,
        "legacy slInit hook installed");
  Check(streamline_internal::g_real_legacy_support == DiscoverySupport &&
        streamline_internal::g_real_legacy_enabled == DiscoveryEnabled &&
        streamline_internal::g_real_legacy_settings == DiscoverySettings,
        "legacy query diagnostics bind the actual bool ABI exports");
  uint32_t discovery_mask = 0xfeed;
  g_legacy_discovery_result = false;
  const auto discovery_calls = g_legacy_discovery_calls;
  Check(!streamline_internal::HookedLegacySupport(0, &discovery_mask) && discovery_mask == 0xfeed &&
        GetLastError() == 1234 && g_legacy_discovery_calls == discovery_calls + 1,
        "support failure preserves mask, false, LastError and single native call");
  g_legacy_discovery_result = true;
  Check(streamline_internal::HookedLegacySupport(0, &discovery_mask) && discovery_mask == 5 &&
        GetLastError() == 1234, "support success preserves adapter mask and bool result");
  Check(streamline_internal::HookedLegacySupport(0, nullptr), "support null mask stays legal");
  Check(streamline_internal::HookedLegacyEnabled(0) && GetLastError() == 1234,
        "enabled query pass-through");
  g_legacy_discovery_result = false;
  Check(!streamline_internal::HookedLegacySettings(0, nullptr, nullptr) && GetLastError() == 1234,
        "settings failure pass-through");
  Check(!streamline_internal::HookedLegacySetEnabled(0, true) && g_set_enabled_argument &&
        GetLastError() == 1234, "feature-enable request and rejected native result both preserved");
  const auto sr_budget = streamline_internal::g_legacy_query_traces[0].load();
  streamline_internal::HookedLegacySupport(3, nullptr);
  Check(streamline_internal::g_legacy_query_traces[0].load() == sr_budget,
        "Reflex query does not exhaust SR diagnostic budget");
  streamline_internal::LegacyPreferences legacy_preferences{};
  Check(streamline_internal::HookedLegacyInit(legacy_preferences, 77) == g_legacy_result &&
            g_legacy_init_calls == 1 && g_legacy_application_id == 77,
        "legacy slInit preserves bool result and application id");

  SetVersion(interposer, 2, 12);
  mock::exports.erase({interposer, "slGetFeatureSettings"});
  mock::exports.erase({interposer, "slSetFeatureConstants"});
  mock::exports[{interposer, "slInit"}] = Proc(RealInit);
  mock::exports[{interposer, "slGetFeatureFunction"}] = Proc(Unrelated);
  mock::exports[{interposer, "slGetFeatureVersion"}] = Proc(Unrelated);
  mock::exports[{interposer, "slIsFeatureSupported"}] = Proc(RealSupport);
  mock::exports[{interposer, "slGetFeatureRequirements"}] = Proc(RealFeatureRequirements);
  mock::exports[{interposer, "slIsFeatureLoaded"}] = Proc(RealFeatureLoaded);
  mock::exports[{interposer, "slSetD3DDevice"}] = Proc(RealSetD3DDevice);
  mock::allocation[reinterpret_cast<const void*>(Proc(RealInit))] = interposer;
  mock::allocation[reinterpret_cast<const void*>(Proc(RealFeatureRequirements))] = interposer;
  SetVersion(interposer, 3, 0);
  Check(streamline_internal::GetInterposerAbi(interposer) == streamline_internal::InterposerAbi::kModern,
        "new file version retains complete modern export contract");
  SetVersion(interposer, 2, 12);
  Check(streamline_internal::GetInterposerAbi(interposer) == streamline_internal::InterposerAbi::kModern &&
            mfgunlock::streamline::CanHookInit(interposer),
        "modern Streamline ABI admitted separately");
  mock::modules.erase(L"sl.common.dll");
  Check(mfgunlock::streamline::CanHookInit(interposer) &&
            !mfgunlock::streamline::FullModernContractReady(interposer),
        "modern init hook precedes common without qualifying full contract");
  mock::modules[L"sl.common.dll"] = Module(0x5152);
  streamline_internal::InstallSupportHook();
  Check(streamline_internal::g_support_hooked.load() &&
            streamline_internal::g_real_support == RealSupport,
        "modern Streamline support hook installed");
  streamline_internal::InstallDiagnosticHooks();
  Check(streamline_internal::g_diagnostic_hooked.load() &&
            streamline_internal::g_real_requirements == RealFeatureRequirements &&
            streamline_internal::g_real_loaded == RealFeatureLoaded &&
            streamline_internal::g_real_set_d3d_device == RealSetD3DDevice,
        "modern Streamline lifecycle diagnostic hooks installed");
  sl::FeatureRequirements feature_requirements{};
  Check(streamline_internal::HookedGetFeatureRequirements(
            sl::kFeatureDLSS_G, feature_requirements) == sl::Result::eOk &&
            g_feature_requirements_calls == 1,
        "DLSS-G feature requirements result preserved");
  bool feature_loaded = false;
  Check(streamline_internal::HookedIsFeatureLoaded(sl::kFeatureDLSS_G, feature_loaded) ==
            sl::Result::eOk && feature_loaded && g_feature_loaded_calls == 1,
        "DLSS-G loaded state observed without modification");
  Check(streamline_internal::HookedSetD3DDevice(reinterpret_cast<void*>(0x1234)) ==
            sl::Result::eOk && g_set_device_calls == 1 &&
            streamline_internal::g_set_device_completed.load(),
        "slSetD3DDevice result preserved and phase recorded");
  mfgunlock::streamline::NotifyD3D12DeviceRecreated();
  Check(streamline_internal::g_d3d12_device_observed.load() &&
            !streamline_internal::g_set_device_active.load() &&
            !streamline_internal::g_set_device_completed.load(),
        "device recreation invalidates Streamline device-owned evidence");
  Check(streamline_internal::HookedSetD3DDevice(reinterpret_cast<void*>(0x5678)) ==
            sl::Result::eOk && g_set_device_calls == 2 &&
            streamline_internal::g_set_device_completed.load(),
        "Streamline device evidence can be rebuilt after recreation");
  sl::AdapterInfo adapter_info{};
  adapter_info.deviceLUID = &g_luid;
  adapter_info.deviceLUIDSizeInBytes = sizeof(g_luid);
  g_support_architecture = 0;
  Check(streamline_internal::HookedIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info) ==
            sl::Result::eOk && g_support_architecture == architecture::kAdaArchitecture,
        "DLSS-G support query uses scoped architecture exposure");
  g_force_support_result = true;
  g_forced_support_result = sl::Result::eErrorNoSupportedAdapterFound;
  Check(streamline_internal::HookedIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info) ==
            sl::Result::eOk,
        "prepared provider relaxes cached Streamline adapter rejection");
  LUID other_luid = g_luid;
  ++other_luid.LowPart;
  sl::AdapterInfo other_adapter{};
  other_adapter.deviceLUID = &other_luid;
  other_adapter.deviceLUIDSizeInBytes = sizeof(other_luid);
  Check(streamline_internal::HookedIsFeatureSupported(sl::kFeatureDLSS_G, other_adapter) ==
            sl::Result::eErrorNoSupportedAdapterFound,
        "Streamline rejection preserved for a different adapter LUID");
  g_forced_support_result = sl::Result::eErrorFeatureMissing;
  Check(streamline_internal::HookedIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info) ==
            sl::Result::eErrorFeatureMissing,
        "Streamline non-adapter failures remain native");
  g_force_support_result = false;
  g_support_architecture = 0;
  Check(streamline_internal::HookedIsFeatureSupported(99, adapter_info) ==
            sl::Result::eErrorFeatureMissing && g_support_architecture == 0,
        "non-DLSS-G support query remains native");

  const auto plugin_module = Module(0x6161);
  mock::paths[plugin_module] = L"X:\\fixture\\sl.dlss_g.dll";
  SetVersion(plugin_module, 99, 42);
  mock::exports[{plugin_module, "slGetPluginFunction"}] = Proc(RealGateway);
  mock::allocation[reinterpret_cast<const void*>(Proc(RealGateway))] = plugin_module;
  mfgunlock::streamline::g_on_dlssg_plugin_bound = ObservePlugin;
  mfgunlock::streamline::g_on_dlssg_plugin_unloaded = ObservePluginUnload;
  Check(streamline_internal::BindPlugin(plugin_module, Proc(RealGateway)) == Proc(RealGateway),
        "plugin binding preserves native gateway pointer");
  Check(g_plugin_bound_calls == 1 && g_bound_plugin == plugin_module,
        "plugin binding publishes the active Streamline module");
  auto& plugin = streamline_internal::g_plugins[0];
  Check(plugin.gateway_hook.installed.load(), "plugin gateway detour installed");
  const auto prior_lookups = g_gateway_lookups;
  Check(prior_lookups != 0, "initial binding probes vendor gateway");
  Check(streamline_internal::BindPlugin(plugin_module, Proc(RealGateway)) == Proc(RealGateway) &&
            g_plugin_bound_calls == 1 && g_gateway_lookups == prior_lookups,
        "repeated plugin attach performs zero vendor lookups and callbacks");
  auto gateway = reinterpret_cast<streamline_internal::GatewayFn>(
      plugin.gateway_hook.replacement.load(std::memory_order_acquire));
  Check(gateway && gateway("slOnPluginLoad") == RealGateway("slOnPluginLoad") &&
            plugin.load_hook.installed.load(),
        "plugin load hook installed through native gateway");
  Check(gateway("slOnPluginStartup") == RealGateway("slOnPluginStartup") &&
            plugin.startup_hook.installed.load(),
        "plugin startup hook installed through native gateway");
  auto load = reinterpret_cast<streamline_internal::LoadFn>(
      plugin.load_hook.replacement.load(std::memory_order_acquire));
  const char* plugin_json = nullptr;
  g_load_architecture = 0;
  Check(load && load(nullptr, "{}", &plugin_json) &&
            g_load_architecture == architecture::kAdaArchitecture &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kLoaded,
        "retargeted plugin load advances lifecycle under scoped architecture exposure");
  auto startup = reinterpret_cast<streamline_internal::StartupFn>(
      plugin.startup_hook.replacement.load(std::memory_order_acquire));
  g_plugin_result = false;
  g_startup_architecture = 0;
  Check(startup && !startup("{}", reinterpret_cast<void*>(0x1234)) &&
            g_startup_architecture == architecture::kAdaArchitecture &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kStartupFailed,
        "plugin startup failure is observed without masking");
  g_plugin_result = true;
  g_startup_architecture = 0;
  Check(startup && startup("{}", reinterpret_cast<void*>(0x1234)) &&
            g_startup_architecture == architecture::kAdaArchitecture &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kStartupSucceeded,
        "plugin startup callback advances lifecycle under diagnostic wrapper");

  mock::module_current[plugin.gateway_hook.identity.module] = false;
  streamline_internal::RefreshPluginLifetimes();
  Check(!plugin.gateway_hook.installed.load() && !plugin.load_hook.installed.load() &&
            !plugin.startup_hook.installed.load() && g_plugin_unloaded_calls == 1 &&
            g_unloaded_plugin == plugin_module &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kUnloaded,
        "plugin disappearance clears lifecycle hooks and publishes unload");
  mock::module_current[reinterpret_cast<HMODULE>(Proc(RealGateway))] = true;
  Check(streamline_internal::BindPlugin(plugin_module, Proc(RealGateway)) == Proc(RealGateway) &&
            plugin.gateway_hook.installed.load() && g_plugin_bound_calls == 2,
        "plugin reload binds a fresh gateway after stale pointers were discarded");

  TestDiscoveryPassThrough();

  mfgunlock::streamline::Shutdown();
  Check(!plugin.gateway_hook.installed.load() && !plugin.load_hook.installed.load() &&
            !plugin.startup_hook.installed.load(),
        "plugin lifecycle hooks detached");
  Check(ngx_internal::g_bound_gpu.load() == nullptr, "NGX adapter binding cleared");

  architecture::Configure(Architecture::kAda);
  g_architecture = architecture::kAdaArchitecture;
  g_arch_calls = 0;
  g_support_architecture = 0;
  g_force_support_result = false;
  g_plugin_result = true;
  g_load_architecture = 0;
  g_startup_architecture = 0;
  g_provider_maintenance_calls = 0;
  mock::module_current[reinterpret_cast<HMODULE>(Proc(RealGateway))] = true;
  ngx_internal::g_bound_gpu = g_gpu;
  mfgunlock::ngx::g_prepare_loaded_providers = PrepareProviders;
  mfgunlock::streamline::Initialize(Module(0x7171));
  Check(streamline_internal::LifecycleEnabled() && !architecture::NeedsBridge(),
        "native Ada observes lifecycle without entering the retarget bridge");

  sl::Preferences ada_preferences{};
  const sl::Feature ada_features[] = {sl::kFeatureDLSS_G};
  ada_preferences.featuresToLoad = ada_features;
  ada_preferences.numFeaturesToLoad = 1;
  g_original_preferences = &ada_preferences;
  const auto maintenance_before_ada_init = g_provider_maintenance_calls;
  Check(mfgunlock::streamline::OnInit(ada_preferences, sl::kSDKVersion, RealInit) == g_sl_result &&
            g_provider_maintenance_calls == maintenance_before_ada_init,
        "Ada slInit observes features without retarget preflight");

  streamline_internal::InstallSupportHook();
  Check(streamline_internal::g_support_hooked.load(),
        "Ada installs the modern support observer without a retarget bridge");
  g_support_architecture = 0;
  Check(streamline_internal::HookedIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info) ==
            sl::Result::eOk && g_support_architecture == architecture::kAdaArchitecture,
        "Ada support query remains native while being observed");

  const auto ada_plugin = Module(0x8181);
  mock::paths[ada_plugin] = L"X:\\fixture\\sl.dlss_g.dll";
  SetVersion(ada_plugin, 2, 14);
  mock::exports[{ada_plugin, "slGetPluginFunction"}] = Proc(RealGateway);
  mock::allocation[reinterpret_cast<const void*>(Proc(RealGateway))] = ada_plugin;
  mfgunlock::streamline::g_on_dlssg_plugin_bound = ObservePlugin;
  Check(streamline_internal::BindPlugin(ada_plugin, Proc(RealGateway)) == Proc(RealGateway),
        "Ada binds the native DLSS-G lifecycle gateway");
  auto& ada_runtime = streamline_internal::g_plugins[0];
  auto ada_gateway = reinterpret_cast<streamline_internal::GatewayFn>(
      ada_runtime.gateway_hook.replacement.load(std::memory_order_acquire));
  Check(ada_gateway && ada_gateway("slOnPluginLoad") == RealGateway("slOnPluginLoad") &&
            ada_gateway("slOnPluginStartup") == RealGateway("slOnPluginStartup"),
        "Ada lifecycle callbacks are observed through the native gateway");
  auto ada_load = reinterpret_cast<streamline_internal::LoadFn>(
      ada_runtime.load_hook.replacement.load(std::memory_order_acquire));
  auto ada_startup = reinterpret_cast<streamline_internal::StartupFn>(
      ada_runtime.startup_hook.replacement.load(std::memory_order_acquire));
  const auto maintenance_before_ada_lifecycle = g_provider_maintenance_calls;
  Check(ada_load && ada_load(nullptr, "{}", &plugin_json) &&
            ada_startup && ada_startup("{}", reinterpret_cast<void*>(0x2222)) &&
            g_load_architecture == architecture::kAdaArchitecture &&
            g_startup_architecture == architecture::kAdaArchitecture &&
            g_provider_maintenance_calls == maintenance_before_ada_lifecycle &&
            streamline_internal::g_lifecycle_state.load() ==
                streamline_internal::LifecycleState::kStartupSucceeded,
        "Ada plugin lifecycle stays native and reaches STARTUP_SUCCEEDED");
  mfgunlock::streamline::Shutdown();
  mfgunlock::streamline::Initialize(Module(0x9191));
  Check(streamline_internal::LifecycleEnabled(),
        "Streamline lifecycle can be initialized again after shutdown");
  mfgunlock::streamline::Shutdown();
  Check(!streamline_internal::g_support_hooked.load() &&
            !streamline_internal::g_diagnostic_hooked.load(),
        "repeated shutdown leaves no duplicate Streamline entry hooks");

  const auto unloaded_ngx = Module(0x9991);
  runtime.module = unloaded_ngx;
  Check(mfgunlock::hook::CaptureModule(unloaded_ngx, runtime.identity),
        "NGX unload fixture has a captured module identity");
  runtime.entry_create = RealCreate;
  mock::module_current[unloaded_ngx] = false;
  mfgunlock::ngx::RefreshEntryLifetimes();
  Check(runtime.module == nullptr && runtime.entry_create == nullptr,
        "periodic NGX lifetime refresh invalidates stale entry pointers");

  g_parameters.values["SuperSampling.Available"] = 0;
  g_parameters.values["SuperSampling.FeatureInitResult"] = 0xbad;
  const auto cap_values = g_parameters.values;
  const auto cap_writes = g_parameters.writes;
  SetLastError(9911);
  ngx_internal::TraceCapabilityBlock("TEST", "NATIVE", &g_parameters, 1);
  Check(g_parameters.values == cap_values && g_parameters.writes == cap_writes && GetLastError() == 9911,
        "capability diagnostic only reads, never converts SR failure into success");
  std::cout << "host wrappers: " << g_checks
            << " checks PASS (mock APIs; no ABI/Detours/GPU test)\n";
  return 0;
}
