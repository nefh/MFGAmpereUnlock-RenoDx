// SPDX-License-Identifier: MIT
// Executes the production Streamline/NGX wrappers against deterministic API
// doubles. This is not a Windows ABI, Detours, GPU, or game test.

#include "streamline_bridge.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

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
NVSDK_NGX_Result g_create_result = NVSDK_NGX_Result_Success;
NVSDK_NGX_Result g_evaluate_result = NVSDK_NGX_Result_Success;
NVSDK_NGX_Result g_release_result = NVSDK_NGX_Result_Success;
NVSDK_NGX_Handle g_handle{99};
bool g_return_handle = true;

NVSDK_NGX_Result RealCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                            NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** output) {
  ++g_creates;
  if (output) *output = g_return_handle ? &g_handle : nullptr;
  return g_create_result;
}

NVSDK_NGX_Result RealEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                              const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  ++g_evaluates;
  return g_evaluate_result;
}

NVSDK_NGX_Result RealRelease(NVSDK_NGX_Handle*) {
  ++g_releases;
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

sl::Result g_sl_result = sl::Result::eErrorNoSupportedAdapterFound;
unsigned int g_init_calls = 0;
const sl::Preferences* g_original_preferences = nullptr;

sl::Result RealInit(const sl::Preferences& preferences, uint64_t) {
  ++g_init_calls;
  Check(&preferences == g_original_preferences, "same preferences reference");
  return g_sl_result;
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
const char* g_plugin_json = "{\"supportedAdapters\":0}";

bool RealLoad(sl::param::IParameters*, const char*, const char** output) {
  ++g_loads;
  if (output) *output = g_plugin_json;
  return g_plugin_result;
}

bool RealStartup(const char*, void*) {
  ++g_startups;
  return g_plugin_result;
}

void Unrelated() {}

void* RealGateway(const char* name) {
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

int main() {
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
            requirements.FeatureSupported == 0 &&
            requirements.MinHWArchitecture == architecture::kAmpereArchitecture,
        "Vulkan requirements use the same policy");

  ResetParameters(0, 1, 0, NVSDK_NGX_Result_FAIL_FeatureNotSupported);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  int value = 0;
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 1,
        "eligible provider exposes frame generation");
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 3,
        "capability maximum raised to verified limit");

  ResetParameters(0, 1, 1);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("FrameGeneration.Available", &value) == NVSDK_NGX_Result_Success && value == 0,
        "explicit driver requirement is preserved");

  ResetParameters(1, 5, 0);
  ngx_internal::ApplyCapabilities(&g_parameters, 3);
  Check(g_parameters.Get("DLSSG.MultiFrameCountMax", &value) == NVSDK_NGX_Result_Success && value == 5,
        "larger native capability is never reduced");

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

  NVSDK_NGX_Handle unrelated{};
  mfgunlock::ngx::g_status.feature_active = false;
  ngx_internal::Evaluate<0>(nullptr, &unrelated, nullptr, nullptr);
  Check(!mfgunlock::ngx::g_status.feature_active.load(), "unrelated Evaluate not marked active");
  ngx_internal::Evaluate<0>(nullptr, &g_handle, nullptr, nullptr);
  Check(mfgunlock::ngx::g_status.feature_active.load(), "tracked FG Evaluate marked active");

  g_release_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  ngx_internal::Release<0>(&g_handle);
  Check(ngx_internal::IsTracked(runtime, &g_handle), "failed Release keeps handle");
  g_release_result = NVSDK_NGX_Result_Success;
  ngx_internal::Release<0>(&g_handle);
  Check(!ngx_internal::IsTracked(runtime, &g_handle), "successful Release removes handle");

  sl::Preferences preferences{};
  preferences.flags = static_cast<sl::PreferenceFlags>(0x69);
  g_original_preferences = &preferences;
  const auto original_flags = preferences.flags;
  Check(mfgunlock::streamline::OnInit(preferences, sl::kSDKVersion, RealInit) == g_sl_result,
        "modern slInit result preserved");
  Check(preferences.flags == original_flags, "modern slInit preferences untouched");

  const auto interposer = Module(0x5151);
  mock::modules[L"sl.interposer.dll"] = interposer;
  mock::paths[interposer] = L"X:\\fixture\\sl.interposer.dll";
  SetVersion(interposer, 1, 5);
  mock::exports[{interposer, "slInit"}] = Proc(RealLegacyInit);
  Check(streamline_internal::GetInterposerAbi(interposer) == streamline_internal::InterposerAbi::kLegacy,
        "Streamline 1.x ABI detected");
  Check(!mfgunlock::streamline::CanHookInit(interposer), "legacy slInit excluded from modern ABI hook");
  streamline_internal::InstallLegacyInitHook();
  Check(streamline_internal::g_legacy_init_hooked.load() && streamline_internal::g_real_legacy_init == RealLegacyInit,
        "legacy slInit hook installed");
  streamline_internal::LegacyPreferences legacy_preferences{};
  Check(streamline_internal::HookedLegacyInit(legacy_preferences, 77) == g_legacy_result &&
            g_legacy_init_calls == 1 && g_legacy_application_id == 77,
        "legacy slInit preserves bool result and application id");

  SetVersion(interposer, 2, 12);
  mock::exports[{interposer, "slGetFeatureFunction"}] = Proc(Unrelated);
  Check(streamline_internal::GetInterposerAbi(interposer) == streamline_internal::InterposerAbi::kModern &&
            mfgunlock::streamline::CanHookInit(interposer),
        "modern Streamline ABI admitted separately");

  const auto plugin_module = Module(0x6161);
  mock::paths[plugin_module] = L"X:\\fixture\\sl.dlss_g.dll";
  SetVersion(plugin_module, 2, 12);
  Check(streamline_internal::BindPlugin(plugin_module, Proc(RealGateway)) == Proc(RealGateway),
        "plugin binding preserves native gateway pointer");
  auto& plugin = streamline_internal::g_plugins[0];
  Check(plugin.gateway_hook.installed.load(), "plugin gateway detour installed");
  auto gateway = reinterpret_cast<streamline_internal::GatewayFn>(
      plugin.gateway_hook.replacement.load(std::memory_order_acquire));
  Check(gateway && gateway("slOnPluginLoad") == RealGateway("slOnPluginLoad") &&
            plugin.load_hook.installed.load(),
        "plugin load hook installed through native gateway");
  Check(gateway("slOnPluginStartup") == RealGateway("slOnPluginStartup") &&
            plugin.startup_hook.installed.load(),
        "plugin startup hook installed through native gateway");

  mfgunlock::streamline::Shutdown();
  Check(!plugin.gateway_hook.installed.load() && !plugin.load_hook.installed.load() &&
            !plugin.startup_hook.installed.load(),
        "plugin lifecycle hooks detached");
  Check(ngx_internal::g_bound_gpu.load() == nullptr, "NGX adapter binding cleared");

  std::cout << "host wrappers: " << g_checks
            << " checks PASS (mock APIs; no ABI/Detours/GPU test)\n";
  return 0;
}
