// SPDX-License-Identifier: MIT
// Executes the production Ampere capability and NGX wrappers against
// deterministic API doubles. This is not a Windows ABI, Detours, GPU, or game test.

#include "ampere_caps.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace mfgunlock;

namespace ampere_ngx = ampere::ngx::internal;
namespace ampere_caps = ampere::caps::internal;

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
NvPhysicalGpuHandle g_second_gpu = Module(0x200);
LUID g_second_luid{11, 20};
unsigned int g_physical_count = 1;
unsigned int g_architecture = ampere::kAmpereArchitecture;
unsigned int g_implementation = 2;
unsigned int g_arch_calls = 0;
NvAPI_Status g_arch_status = 0;

NvAPI_Status EnumGpu(NvPhysicalGpuHandle* output, NvU32* count) {
  output[0] = g_gpu;
  if (g_physical_count > 1) output[1] = g_second_gpu;
  *count = g_physical_count;
  return 0;
}

NvAPI_Status GpuLuid(NvPhysicalGpuHandle gpu, LUID* output) {
  *output = gpu == g_second_gpu ? g_second_luid : g_luid;
  return 0;
}

NvAPI_Status GpuArch(NvPhysicalGpuHandle, NV_GPU_ARCH_INFO* output) {
  ++g_arch_calls;
  if (g_arch_status != 0 || output == nullptr) return g_arch_status;

  output->architecture = g_architecture;
  output->implementation = g_implementation;
  output->revision = 0xa1;
  return 0;
}

unsigned int g_requirement_calls = 0;
unsigned int g_entry_requirement_calls = 0;
unsigned int g_requirement_flags = ampere::kAdapterUnsupported;
unsigned int g_requirement_architecture = ampere::kAdaArchitecture;
NVSDK_NGX_Result g_requirement_result = NVSDK_NGX_Result_Success;

NVSDK_NGX_Result RealRequirements(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*,
                                  NVSDK_NGX_FeatureRequirement* output) {
  ++g_requirement_calls;
  if (output != nullptr && g_requirement_result == NVSDK_NGX_Result_Success) {
    output->FeatureSupported = g_requirement_flags;
    output->MinHWArchitecture = g_requirement_architecture;
    std::strcpy(output->MinOSVersion, "10.0.19041");
  }
  return g_requirement_result;
}

NVSDK_NGX_Result EntryRequirements(IDXGIAdapter* adapter,
                                   const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
                                   NVSDK_NGX_FeatureRequirement* output) {
  ++g_entry_requirement_calls;
  return RealRequirements(adapter, discovery, output);
}

unsigned int g_vulkan_requirement_calls = 0;

NVSDK_NGX_Result RealVulkanRequirements(VkInstance, VkPhysicalDevice,
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
  if (output != nullptr) *output = g_return_handle ? &g_handle : nullptr;
  return g_create_result;
}

NVSDK_NGX_Result RealEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                              const NVSDK_NGX_Parameter*,
                              PFN_NVSDK_NGX_ProgressCallback) {
  ++g_evaluates;
  return g_evaluate_result;
}

NVSDK_NGX_Result RealRelease(NVSDK_NGX_Handle*) {
  ++g_releases;
  return g_release_result;
}

sl::Result g_sl_result = sl::Result::eErrorNoSupportedAdapterFound;
unsigned int g_support_calls = 0;
unsigned int g_loaded_calls = 0;
unsigned int g_sl_requirement_calls = 0;
unsigned int g_version_calls = 0;
unsigned int g_init_calls = 0;

sl::Result RealSupport(sl::Feature, const sl::AdapterInfo&) {
  ++g_support_calls;
  return g_sl_result;
}

sl::Result RealLoaded(sl::Feature, bool& loaded) {
  ++g_loaded_calls;
  loaded = false;
  return g_sl_result;
}

sl::Result RealSlRequirements(sl::Feature, sl::FeatureRequirements& output) {
  ++g_sl_requirement_calls;
  output.flags = 0x123;
  output.payload.fill(0x55);
  return g_sl_result;
}

sl::Result RealVersion(sl::Feature, sl::FeatureVersion& output) {
  ++g_version_calls;
  output.payload.fill(0xabc);
  return g_sl_result;
}

const sl::Preferences* g_original_preferences = nullptr;

sl::Result RealInit(const sl::Preferences& preferences, uint64_t) {
  ++g_init_calls;
  Check(&preferences == g_original_preferences, "same preferences reference");
  return g_sl_result;
}

bool g_plugin_result = false;
unsigned int g_loads = 0;
unsigned int g_startups = 0;
unsigned int g_shutdowns = 0;
const char* g_plugin_json = "{\"supportedAdapters\":0}";

bool RealLoad(sl::param::IParameters*, const char*, const char** output) {
  ++g_loads;
  if (output != nullptr) *output = g_plugin_json;
  return g_plugin_result;
}

bool RealStartup(const char*, void*) {
  ++g_startups;
  return g_plugin_result;
}

void RealShutdown() {
  ++g_shutdowns;
}

void Unrelated() {}

void* RealGateway(const char* name) {
  if (name == nullptr) return nullptr;
  if (std::strcmp(name, "slOnPluginLoad") == 0) return reinterpret_cast<void*>(&RealLoad);
  if (std::strcmp(name, "slOnPluginStartup") == 0)
    return reinterpret_cast<void*>(&RealStartup);
  if (std::strcmp(name, "slOnPluginShutdown") == 0)
    return reinterpret_cast<void*>(&RealShutdown);
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

}  // namespace

int main() {
  mfgunlock::g_enabled = true;
  architecture::Configure(Architecture::kAmpere);
  ampere::g_test_prepared_count = 1;

  auto& nvapi = ampere_ngx::GetNvapi();
  nvapi.enumerate = EnumGpu;
  nvapi.adapter_id = GpuLuid;
  nvapi.architecture = GpuArch;
  nvapi.ready = true;

  IDXGIAdapter adapter{};
  adapter.desc.VendorId = ampere::kNvidiaVendorId;
  adapter.desc.AdapterLuid = g_luid;
  g_arch_status = -10;
  Check(ampere_ngx::MatchAdapter(&adapter) == g_gpu, "physical LUID match");
  Check(g_arch_calls == 0, "explicit profile does not query architecture");
  g_arch_status = 0;

  g_physical_count = 2;
  Check(ampere_ngx::MatchAdapter(&adapter) == g_gpu, "bound adapter reused");
  g_physical_count = 1;

  adapter.desc.AdapterLuid.LowPart = 100;
  Check(ampere_ngx::MatchAdapter(&adapter) == nullptr, "different LUID rejected");
  adapter.desc.AdapterLuid = g_luid;

  NV_GPU_ARCH_INFO arch_info{};
  arch_info.version = NV_GPU_ARCH_INFO_VER;
  ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
  Check(arch_info.architecture == ampere::kAmpereArchitecture,
        "no NVAPI spoof outside scope");

  {
    ampere_ngx::ScopedArchQuery scope(g_gpu);
    ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
    Check(arch_info.architecture == ampere::kAdaArchitecture &&
              arch_info.implementation == 2 && arch_info.revision == 0xa1,
          "only architecture changed");

    ampere_ngx::HookedGetArchInfo(Module(9), &arch_info);
    Check(arch_info.architecture == ampere::kAmpereArchitecture,
          "other handle left alone");

    ampere::g_test_prepared_count = 0;
    ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
    Check(arch_info.architecture == ampere::kAmpereArchitecture, "provider required");
    ampere::g_test_prepared_count = 1;

    g_arch_status = -10;
    Check(ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info) == -10,
          "native NVAPI error preserved");
    g_arch_status = 0;

    {
      ampere_ngx::ScopedArchQuery no_spoof(nullptr);
      ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
      Check(arch_info.architecture == ampere::kAmpereArchitecture,
            "nested non-FG scope cleared");
    }

    ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
    Check(arch_info.architecture == ampere::kAdaArchitecture, "nested scope restored");
  }
  Check(ampere_ngx::g_arch_scope == nullptr, "scope unwound");

  auto& runtime = ampere_ngx::g_slots[0];
  runtime.requirements = RealRequirements;
  runtime.vulkan_requirements = RealVulkanRequirements;
  runtime.create = RealCreate;
  runtime.evaluate = RealEvaluate;
  runtime.release = RealRelease;

  NVSDK_NGX_FeatureDiscoveryInfo discovery{};
  discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
  NVSDK_NGX_FeatureRequirement requirements{};

  for (unsigned int flags = 0; flags < 64; ++flags) {
    g_requirement_flags = flags;
    g_requirement_architecture = ampere::kAdaArchitecture;
    const auto calls_before = g_requirement_calls;

    Check(ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements) ==
              NVSDK_NGX_Result_Success,
          "NGX result preserved");

    const bool expected_override = flags == 0 || flags == ampere::kAdapterUnsupported;
    Check(requirements.MinHWArchitecture ==
              (expected_override ? ampere::kAmpereArchitecture : ampere::kAdaArchitecture),
          "only reviewed architecture correction");
    Check(requirements.FeatureSupported == (expected_override ? 0u : flags),
          "other flags unchanged");
    Check(std::strcmp(requirements.MinOSVersion, "10.0.19041") == 0,
          "OS requirement unchanged");
    Check(g_requirement_calls == calls_before + 1, "one original call");
  }

  g_requirement_flags = ampere::kAdapterUnsupported;
  g_requirement_architecture = ampere::kAmpereArchitecture;
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(requirements.FeatureSupported == 0, "post-retarget requirement handled");

  g_requirement_architecture = ampere::kAdaArchitecture;
  discovery.FeatureID = static_cast<NVSDK_NGX_Feature>(1);
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(requirements.FeatureSupported == ampere::kAdapterUnsupported &&
            requirements.MinHWArchitecture == ampere::kAdaArchitecture,
        "non-FG preserved");
  discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;

  g_requirement_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  requirements.MinHWArchitecture = 123;
  Check(ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements) ==
                g_requirement_result &&
            requirements.MinHWArchitecture == 123,
        "failure output not rewritten");
  g_requirement_result = NVSDK_NGX_Result_Success;

  ampere::g_test_prepared_count = 1;
  ampere::g_test_expired_count = 1;
  g_requirement_flags = ampere::kAdapterUnsupported;
  g_requirement_architecture = ampere::kAdaArchitecture;
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(requirements.FeatureSupported == 0 &&
            requirements.MinHWArchitecture == ampere::kAmpereArchitecture,
        "expired candidate cannot poison a prepared provider");

  ampere::g_test_blocking_count = 1;
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(requirements.FeatureSupported == ampere::kAdapterUnsupported &&
            requirements.MinHWArchitecture == ampere::kAdaArchitecture,
        "real active rejection still blocks");
  Check(std::strcmp(ampere::ngx::g_telemetry.decision.load(),
                    "active-provider-rejected-or-invalidated") == 0,
        "precise blocking reason");
  ampere::g_test_blocking_count = 0;
  ampere::g_test_expired_count = 0;

  runtime.entry_requirements = EntryRequirements;
  const auto requirement_calls_before = g_requirement_calls;
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(g_entry_requirement_calls == 1 &&
            g_requirement_calls == requirement_calls_before + 1,
        "entry trampoline preferred once");
  runtime.entry_requirements = nullptr;

  ampere_ngx::g_bound_gpu = g_gpu;
  g_requirement_flags = ampere::kAdapterUnsupported;
  g_requirement_architecture = ampere::kAdaArchitecture;
  const auto vulkan_calls_before = g_vulkan_requirement_calls;
  ampere_ngx::VulkanRequirements<0>(nullptr, nullptr, &discovery, &requirements);
  Check(g_vulkan_requirement_calls == vulkan_calls_before + 1 &&
            requirements.FeatureSupported == 0 &&
            requirements.MinHWArchitecture == ampere::kAmpereArchitecture,
        "Vulkan NGX requirements use the same architecture policy");

  NVSDK_NGX_Handle* output_handle = nullptr;
  g_create_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  Check(ampere_ngx::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr,
                              &output_handle) == g_create_result,
        "Create failure preserved");
  Check(ampere::ngx::g_telemetry.creates_ok == 0, "failed Create not success");

  g_create_result = NVSDK_NGX_Result_Success;
  g_return_handle = false;
  ampere_ngx::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr,
                        &output_handle);
  Check(ampere::ngx::g_telemetry.creates_ok == 0, "null handle not success");

  g_return_handle = true;
  ampere_ngx::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr,
                        &output_handle);
  Check(ampere_ngx::IsTracked(runtime, &g_handle), "FG handle tracked");

  g_evaluate_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  Check(ampere_ngx::Evaluate<0>(nullptr, &g_handle, nullptr, nullptr) == g_evaluate_result,
        "Evaluate error preserved");
  Check(ampere::ngx::g_telemetry.evaluates == 1, "FG evaluation counted");

  NVSDK_NGX_Handle unrelated{};
  ampere_ngx::Evaluate<0>(nullptr, &unrelated, nullptr, nullptr);
  Check(ampere::ngx::g_telemetry.evaluates == 1, "non-FG evaluation not counted");

  g_release_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  ampere_ngx::Release<0>(&g_handle);
  Check(ampere_ngx::IsTracked(runtime, &g_handle), "failed release keeps handle");

  g_release_result = NVSDK_NGX_Result_Success;
  ampere_ngx::Release<0>(&g_handle);
  Check(!ampere_ngx::IsTracked(runtime, &g_handle), "successful release removes handle");

  ampere_caps::g_real_support = RealSupport;
  ampere_caps::g_real_loaded = RealLoaded;
  ampere_caps::g_real_requirements = RealSlRequirements;
  ampere_caps::g_real_version = RealVersion;

  for (auto result : {sl::Result::eOk, sl::Result::eErrorNoSupportedAdapterFound,
                      sl::Result::eErrorNotInitialized}) {
    g_sl_result = result;
    for (auto feature : {sl::kFeatureDLSS_G, sl::Feature(0)}) {
      bool loaded = true;
      sl::FeatureRequirements feature_requirements{};
      sl::FeatureVersion feature_version{};
      sl::AdapterInfo adapter_info{};

      Check(ampere_caps::HookedIsFeatureSupported(feature, adapter_info) == result,
            "native supported result unchanged");
      Check(ampere_caps::HookedIsFeatureLoaded(feature, loaded) == result && !loaded,
            "native loaded=false never spoofed");
      Check(ampere_caps::HookedGetFeatureRequirements(feature, feature_requirements) == result &&
                feature_requirements.flags == 0x123 &&
                feature_requirements.payload.back() == 0x55,
            "SL requirements never rewritten");
      Check(ampere_caps::HookedGetFeatureVersion(feature, feature_version) == result &&
                feature_version.payload.back() == 0xabc,
            "version never fabricated");
    }
  }

  sl::Preferences preferences{};
  preferences.flags = static_cast<sl::PreferenceFlags>(0x69);
  g_original_preferences = &preferences;
  const auto original_flags = preferences.flags;
  Check(ampere::caps::OnInit(preferences, sl::kSDKVersion, RealInit) == g_sl_result,
        "init error preserved");
  Check(preferences.flags == original_flags, "OTA flags preserved");

  hook::UninstallAddress(ampere_ngx::g_arch_hook);
  constexpr uint64_t kOlderStreamlineSdk = (1ull << 48) | (5ull << 32) | 4ull;
  Check(ampere::caps::OnInit(preferences, kOlderStreamlineSdk, RealInit) == g_sl_result &&
            ampere_ngx::g_arch_hook.installed.load(),
        "older Streamline slInit still runs capability preflight");
  Check(preferences.flags == original_flags, "older Streamline preferences remain untouched");

  auto plugin_module = Module(0x1234);
  SetVersion(plugin_module, 1, 5);
  auto plugin_version = ampere_caps::ReadModuleVersion(plugin_module);
  Check(plugin_version.major == 1 && plugin_version.minor == 5,
        "older Streamline version remains diagnostic data");

  const auto full_resource = mock::resources[plugin_module];
  for (size_t size = 0; size < full_resource.size(); ++size) {
    mock::resources[plugin_module] = {full_resource.begin(), full_resource.begin() + size};
    plugin_version = ampere_caps::ReadModuleVersion(plugin_module);
    Check(plugin_version.major == 0 && plugin_version.minor == 0,
          "truncated version resource rejected safely");
  }
  mock::resources.erase(plugin_module);

  // Version information is diagnostic only. The native function table is the
  // admission check for the DLSS-G plugin, even without version metadata.
  auto bound = ampere_caps::BindPlugin(plugin_module, Proc(RealGateway));
  Check(bound == Proc(RealGateway), "native gateway pointer preserved");
  auto& plugin = ampere_caps::g_plugins[0];
  Check(plugin.gateway_hook.installed.load() &&
            plugin.gateway_hook.Targets(reinterpret_cast<void*>(Proc(RealGateway))),
        "native gateway entry hook installed");
  auto gateway = reinterpret_cast<ampere_caps::GatewayFn>(
      plugin.gateway_hook.replacement.load(std::memory_order_acquire));
  Check(gateway != nullptr, "gateway detour captured");
  Check(!plugin.running.load(), "DLL presence not startup");

  Check(gateway("slOnPluginLoad") == RealGateway("slOnPluginLoad"),
        "native load pointer preserved");
  Check(gateway("slOnPluginStartup") == RealGateway("slOnPluginStartup"),
        "native startup pointer preserved");
  Check(gateway("slOnPluginShutdown") == RealGateway("slOnPluginShutdown"),
        "native shutdown pointer preserved");
  Check(plugin.load_hook.installed.load() && plugin.startup_hook.installed.load() &&
            plugin.shutdown_hook.installed.load(),
        "native lifecycle entry hooks installed");

  auto on_load = reinterpret_cast<ampere_caps::LoadFn>(
      plugin.load_hook.replacement.load(std::memory_order_acquire));
  auto on_startup = reinterpret_cast<ampere_caps::StartupFn>(
      plugin.startup_hook.replacement.load(std::memory_order_acquire));
  auto on_shutdown = reinterpret_cast<ampere_caps::ShutdownFn>(
      plugin.shutdown_hook.replacement.load(std::memory_order_acquire));
  Check(on_load != nullptr && on_startup != nullptr && on_shutdown != nullptr,
        "lifecycle detours captured");
  Check(gateway("slDLSSGSetOptions") == RealGateway("slDLSSGSetOptions"),
        "native options not replaced with dummy");

  const char* returned_json = nullptr;
  Check(!on_load(nullptr, "{}", &returned_json) && returned_json == g_plugin_json,
        "original failed load and JSON preserved");
  Check(!on_startup("{}", nullptr) && !ampere_caps::g_plugins[0].running.load(),
        "failed startup not active");

  g_plugin_result = true;
  Check(on_load(nullptr, "{}", &returned_json), "real load success");
  Check(on_startup("{}", nullptr) && ampere_caps::g_plugins[0].running.load(),
        "real startup success");
  on_shutdown();
  Check(!ampere_caps::g_plugins[0].running.load() && g_shutdowns == 1, "real shutdown");

  ampere_caps::g_self = Module(88);
  const void* caller = reinterpret_cast<const void*>(100);
  mock::allocation[caller] = ampere_caps::g_self;
  Check(ampere::caps::Resolve(plugin_module, "slGetPluginFunction", Proc(RealGateway), caller) ==
            Proc(RealGateway),
        "own Detours installer bypasses wrappers");
  Check(ampere::caps::Resolve(plugin_module, reinterpret_cast<const char*>(2), Proc(Unrelated),
                              nullptr) == Proc(Unrelated),
        "ordinal preserved");

  // Binding and trampoline installation are exercised with a deterministic
  // hook double; no machine-code patch is made by this test.
  auto ngx_module = Module(0x9999);
  mock::paths[ngx_module] = L"X:\\fixture\\_nvngx.dll";
  mock::modules[L"_nvngx.dll"] = ngx_module;
  mock::exports[{ngx_module, "NVSDK_NGX_D3D12_GetFeatureRequirements"}] =
      Proc(RealRequirements);
  mock::exports[{ngx_module, "NVSDK_NGX_D3D12_CreateFeature"}] = Proc(RealCreate);
  mock::exports[{ngx_module, "NVSDK_NGX_D3D12_EvaluateFeature"}] = Proc(RealEvaluate);
  mock::exports[{ngx_module, "NVSDK_NGX_D3D12_ReleaseFeature"}] = Proc(RealRelease);
  mock::exports[{ngx_module, "NVSDK_NGX_VULKAN_GetFeatureRequirements"}] =
      Proc(RealVulkanRequirements);

  auto resolved = ampere::ngx::Resolve(ngx_module, "NVSDK_NGX_D3D12_GetFeatureRequirements",
                                       Proc(RealRequirements));
  Check(resolved == Proc(RealRequirements), "NGX resolver preserves native pointer");
  Check(runtime.requirements == RealRequirements, "NGX native entry recorded");

  mock::install_ok = false;
  ampere::ngx::EnsureEntryHooks();
  Check(!runtime.entries_installed && runtime.entry_requirements == nullptr,
        "failed entry install leaves resolver usable");

  mock::install_ok = true;
  ampere::ngx::EnsureEntryHooks();
  Check(runtime.entries_installed && runtime.entry_requirements != nullptr,
        "D3D12 entry coverage installed");
  Check(runtime.vulkan_requirements_installed && runtime.entry_vulkan_requirements != nullptr,
        "Vulkan requirements entry installed");

  auto vulkan_only_module = Module(0x7777);
  mock::paths[vulkan_only_module] = L"X:\\fixture\\nvngx.dll";
  mock::modules[L"nvngx.dll"] = vulkan_only_module;
  mock::exports[{vulkan_only_module, "NVSDK_NGX_VULKAN_GetFeatureRequirements"}] =
      Proc(RealVulkanRequirements);
  ampere::ngx::EnsureEntryHooks();
  Check(ampere_ngx::g_slots[1].vulkan_requirements_installed &&
            !ampere_ngx::g_slots[1].entries_installed,
        "Vulkan requirements do not depend on D3D12 exports");

  unsigned int install_calls = mock::installs;
  ampere::ngx::EnsureEntryHooks();
  Check(install_calls == mock::installs, "entry install idempotent");

  auto streamline_module = Module(0x8888);
  mock::modules[L"sl.interposer.dll"] = streamline_module;
  SetVersion(streamline_module, 1, 5);
  Check(!ampere::caps::CanHookInit(streamline_module), "slInit requires the actual export");
  mock::exports[{streamline_module, "slInit"}] = Proc(RealInit);
  Check(ampere::caps::CanHookInit(streamline_module),
        "slInit admission depends on the export, not the Streamline version");
  mock::exports[{streamline_module, "slIsFeatureSupported"}] = Proc(RealSupport);

  ampere_caps::g_real_support = nullptr;
  ampere_caps::g_real_loaded = nullptr;
  ampere_caps::g_real_requirements = nullptr;
  ampere_caps::g_real_version = nullptr;
  ampere_caps::g_observers_installed = false;
  ampere_caps::InstallObservers();
  Check(ampere_caps::g_observers_installed.load() && ampere_caps::g_real_support != nullptr &&
            ampere_caps::g_real_loaded == nullptr && ampere_caps::g_real_requirements == nullptr &&
            ampere_caps::g_real_version == nullptr,
        "available Streamline observer installs without a version whitelist");

  mock::exports[{streamline_module, "slIsFeatureLoaded"}] = Proc(RealLoaded);
  mock::exports[{streamline_module, "slGetFeatureRequirements"}] = Proc(RealSlRequirements);
  mock::exports[{streamline_module, "slGetFeatureVersion"}] = Proc(RealVersion);
  ampere_caps::InstallObservers();
  Check(ampere_caps::g_real_loaded != nullptr && ampere_caps::g_real_requirements != nullptr &&
            ampere_caps::g_real_version != nullptr,
        "later Streamline exports are picked up independently");

  install_calls = mock::installs;
  ampere_caps::InstallObservers();
  Check(mock::installs == install_calls, "SL observers install idempotent");

  ampere::caps::Draw();
  Check(g_loads == 2 && g_startups == 2 && g_init_calls == 2,
        "lifecycle calls reach the native functions exactly once per invocation");

  // Tear down the entry hooks and check that native resolver results remain usable.
  auto native_gateway = reinterpret_cast<ampere_caps::GatewayFn>(bound);
  ampere::caps::Shutdown();
  Check(!plugin.gateway_hook.installed.load() && !plugin.load_hook.installed.load() &&
            !plugin.startup_hook.installed.load() && !plugin.shutdown_hook.installed.load(),
        "lifecycle hooks detached");
  Check(native_gateway("slDLSSGSetOptions") == RealGateway("slDLSSGSetOptions"),
        "cached native gateway remains valid after detach");
  Check(!runtime.entries_installed && !runtime.vulkan_requirements_installed &&
            ampere_ngx::g_bound_gpu.load() == nullptr,
        "NGX hooks and adapter binding cleared");
  ampere::ngx::g_shutting_down = false;

  architecture::Configure(Architecture::kAuto);
  g_physical_count = 1;
  g_arch_status = 0;
  g_architecture = ampere::kAmpereArchitecture;
  const auto auto_detect_calls = g_arch_calls;
  ampere::ngx::DetectArchitecture();
  Check(architecture::ActiveProfile() == &architecture::kAmpere &&
            ampere_ngx::g_bound_gpu.load() == nullptr &&
            g_arch_calls == auto_detect_calls + 1,
        "Auto detects a single NVIDIA GPU without Streamline or an NGX adapter binding");

  // Exercise the same real wrappers for both explicit backport profiles.
  runtime.requirements = RealRequirements;
  g_requirement_result = NVSDK_NGX_Result_Success;
  g_requirement_flags = ampere::kAdapterUnsupported;
  g_requirement_architecture = ampere::kAdaArchitecture;
  g_architecture = ampere::kTuringArchitecture;
  for (auto selected : {Architecture::kAmpere, Architecture::kTuring}) {
    architecture::Configure(selected);
    ampere_ngx::g_bound_gpu = nullptr;
    const auto* profile = architecture::ActiveProfile();
    const auto arch_calls_before = g_arch_calls;
    ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
    Check(g_arch_calls == arch_calls_before, "manual NGX path does not detect architecture");
    Check(requirements.MinHWArchitecture == profile->native_arch && requirements.FeatureSupported == 0,
          "NGX uses explicit target");
    {
      ampere_ngx::ScopedArchQuery scope(g_gpu);
      ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
      Check(arch_info.architecture == profile->exposed_arch, "scoped target exposure");
    }
    ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
    Check(arch_info.architecture == g_architecture, "real arch remains visible outside scope");
    hook::UninstallAddress(ampere_ngx::g_arch_hook);
  }

  architecture::Configure(Architecture::kAda);
  const auto calls_before_ada = g_arch_calls;
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(requirements.FeatureSupported == g_requirement_flags &&
            requirements.MinHWArchitecture == g_requirement_architecture,
        "Ada leaves NGX requirements untouched");
  Check(g_arch_calls == calls_before_ada, "Ada does not inspect hardware");

  for (const auto* profile : {&architecture::kAda, &architecture::kAmpere, &architecture::kTuring}) {
    architecture::Configure(Architecture::kAuto);
    ampere_ngx::g_bound_gpu = nullptr;
    g_architecture = profile->native_arch;
    auto before = g_arch_calls;
    Check(ampere_ngx::MatchAdapter(&adapter) == g_gpu, "Auto binds actual NGX adapter");
    Check(architecture::ActiveProfile() == profile && g_arch_calls == before + 1,
          "Auto reads original architecture once");
    // The hook may expose Ada, but a repeated lookup must not detect that as the GPU.
    {
      ampere_ngx::ScopedArchQuery scope(g_gpu);
      ampere_ngx::HookedGetArchInfo(g_gpu, &arch_info);
      before = g_arch_calls;
      Check(ampere_ngx::MatchAdapter(&adapter) == g_gpu && g_arch_calls == before &&
                architecture::ActiveProfile() == profile,
            "Auto uses cached identity inside spoof scope");
    }
  }
  architecture::Configure(Architecture::kAuto);
  ampere_ngx::g_bound_gpu = nullptr;
  g_arch_status = -10;
  Check(!ampere_ngx::MatchAdapter(&adapter) && architecture::NeedsDetection(),
        "failed NVAPI query does not select a profile");
  g_arch_status = 0;
  g_architecture = 0x1b0;
  Check(ampere_ngx::MatchAdapter(&adapter) == g_gpu && !architecture::ActiveProfile(),
        "unknown Auto architecture has no fallback");
  const auto previous_overrides = ampere::ngx::g_telemetry.overrides.load();
  ampere_ngx::Requirements<0>(&adapter, &discovery, &requirements);
  Check(ampere::ngx::g_telemetry.overrides.load() == previous_overrides &&
            requirements.FeatureSupported == g_requirement_flags,
        "unknown architecture leaves native requirements alone");

  // Multi-GPU Auto waits for the actual device rather than choosing adapter 0.
  architecture::Configure(Architecture::kAuto);
  ampere_ngx::g_bound_gpu = nullptr;
  g_physical_count = 2;
  g_architecture = 0x160;
  const auto calls_before_multi = g_arch_calls;
  ampere_ngx::ProbeAdapterBeforeInit();
  Check(architecture::NeedsDetection() && !ampere_ngx::g_bound_gpu.load() &&
            g_arch_calls == calls_before_multi, "multi-GPU startup defers architecture detection");
  adapter.desc.AdapterLuid = g_second_luid;
  Check(ampere_ngx::MatchAdapter(&adapter) == g_second_gpu &&
            architecture::ActiveProfile() == &architecture::kTuring,
        "Auto binds requested second adapter");
  adapter.desc.AdapterLuid = g_luid;
  Check(!ampere_ngx::MatchAdapter(&adapter), "another adapter cannot replace the active binding");
  g_physical_count = 1;

  // An Ada profile must not add the backport lifecycle or architecture hooks.
  architecture::Configure(Architecture::kAda);
  ampere_caps::g_shutting_down = false;
  const auto installs_before_ada = mock::installs;
  Check(ampere::caps::Resolve(plugin_module, "slGetPluginFunction", Proc(RealGateway), nullptr) ==
            Proc(RealGateway) && !ampere_caps::g_plugins[0].module &&
            mock::installs == installs_before_ada, "Ada resolver remains native");

  std::cout << "host wrappers: " << g_checks
            << " checks PASS (mock APIs; no ABI/Detours/GPU test)\n";
  return 0;
}
