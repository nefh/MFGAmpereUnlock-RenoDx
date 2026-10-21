cmake_minimum_required(VERSION 3.24)
project(mfgampereunlock_validation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

# Assertions are part of the test oracle, including Release/MSVC builds. Never
# let NDEBUG turn a passing executable into an executable that checks nothing.
if(MSVC)
  add_compile_options(/UNDEBUG)
else()
  add_compile_options(-UNDEBUG)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tests")

# Validation ReShade doubles intentionally omit RESHADE_API_VERSION while
# exposing the modern logging/config surface. Pin only that mock ABI here so
# the production compatibility shim does not select legacy wrappers around the
# doubles. Real addon builds still detect these values from the selected SDK.
add_compile_definitions(
  MFGUNLOCK_RESHADE_LOG_STYLE=2
  MFGUNLOCK_RESHADE_CONFIG_STYLE=1)

foreach(test assertions_enabled_test addon_entry_test registration_trace_test architecture_test feature_capabilities_test early_load_test ptx_retarget_test temporal_composition_test runtime_controls_test capability_policy_test output_state_test presentation_observer_test runtime_lifecycle_test advanced_features_test composition_state_test ui_alpha_test ui_submission_test ui_alpha_backend_test)
  add_executable(${test} "${TEST_ROOT}/${test}.cpp")
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# A process-lifetime host reference protects all SDK-cached entrypoints.
add_executable(host_lifetime_test "${TEST_ROOT}/host_lifetime_test.cpp")
if(WIN32)
  add_library(host_lifetime_fixture SHARED "${TEST_ROOT}/host_lifetime_fixture.cpp")
  add_dependencies(host_lifetime_test host_lifetime_fixture)
  add_test(NAME host_lifetime_test COMMAND host_lifetime_test "$<TARGET_FILE:host_lifetime_fixture>")
else()
  add_test(NAME host_lifetime_test COMMAND host_lifetime_test)
endif()

# The host-wrapper test compiles the real production headers against explicit
# API doubles. It is intentionally non-Windows: the real addon build uses the
# NVIDIA/Windows/ReShade headers instead of these mocks.
if(NOT WIN32)
  set(mock_dir "${CMAKE_CURRENT_BINARY_DIR}/host-mock-include")
  file(MAKE_DIRECTORY "${mock_dir}")
  file(COPY "${TEST_ROOT}/host_mocks/vulkan" DESTINATION "${mock_dir}")
  file(GLOB mock_headers "${TEST_ROOT}/host_mocks/*.h" "${TEST_ROOT}/host_mocks/*.hpp")
  foreach(header IN LISTS mock_headers)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "${mock_dir}/${name}" COPYONLY)
  endforeach()
  foreach(name vulkan_contract.hpp vulkan_native.hpp architecture.hpp provider_profile.hpp feature_capabilities.hpp compatibility_foundation.hpp compatibility_runtime.hpp streamline_bridge.hpp ngx_bridge.hpp loadhook.hpp capability_policy.hpp
               framecount.hpp runtime_controls.hpp ui_alpha.hpp ui_alpha_backend.hpp ui_submission.hpp ui_candidate.hpp observer_scope.hpp count_observer.hpp diagnostic_bridge.hpp integration_contract.hpp output_state.hpp presentation_observer.hpp quality_config.hpp advanced_features.hpp)
    configure_file("${REPO_ROOT}/src/addons/mfgunlock/${name}" "${mock_dir}/${name}" COPYONLY)
  endforeach()
  configure_file("${REPO_ROOT}/src/addons/mfgunlock/quality_guard.hpp"
                 "${mock_dir}/quality_guard.hpp" COPYONLY)
  configure_file("${REPO_ROOT}/src/addons/mfgunlock/reshade_compat.hpp"
                 "${mock_dir}/reshade_compat.hpp" COPYONLY)
  add_executable(loadhook_notifications_test "${TEST_ROOT}/loadhook_notifications_test.cpp")
  target_include_directories(loadhook_notifications_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME loadhook_notifications_test COMMAND loadhook_notifications_test)

  add_executable(host_wrappers_test "${TEST_ROOT}/host_wrappers_test.cpp")
  target_include_directories(host_wrappers_test PRIVATE "${mock_dir}")
  add_test(NAME host_wrappers_test COMMAND host_wrappers_test)

  add_executable(ngx_feature_generation_test "${TEST_ROOT}/ngx_feature_generation_test.cpp")
  target_include_directories(ngx_feature_generation_test PRIVATE "${mock_dir}")
  add_test(NAME ngx_feature_generation_test COMMAND ngx_feature_generation_test)

  add_executable(vulkan_native_test "${TEST_ROOT}/vulkan_native_test.cpp")
  target_include_directories(vulkan_native_test PRIVATE "${mock_dir}")
  add_test(NAME vulkan_native_test COMMAND vulkan_native_test)

  add_executable(vulkan_wrappers_test "${TEST_ROOT}/vulkan_wrappers_test.cpp")
  target_include_directories(vulkan_wrappers_test PRIVATE "${mock_dir}")
  add_test(NAME vulkan_wrappers_test COMMAND vulkan_wrappers_test)

  add_executable(runtime_settings_test "${TEST_ROOT}/runtime_settings_test.cpp")
  target_include_directories(runtime_settings_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME runtime_settings_test COMMAND runtime_settings_test)

  add_executable(framecount_profile_test "${TEST_ROOT}/framecount_profile_test.cpp")
  target_include_directories(framecount_profile_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME framecount_profile_test COMMAND framecount_profile_test)

  add_executable(diagnostics_dynamic_probe_test "${TEST_ROOT}/diagnostics_dynamic_probe_test.cpp")
  target_include_directories(diagnostics_dynamic_probe_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME diagnostics_dynamic_probe_test COMMAND diagnostics_dynamic_probe_test)

  add_executable(quality_guard_test "${TEST_ROOT}/quality_guard_test.cpp")
  target_include_directories(quality_guard_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME quality_guard_test COMMAND quality_guard_test)

  add_executable(ui_history_isolation_test "${TEST_ROOT}/ui_history_isolation_test.cpp")
  target_include_directories(ui_history_isolation_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME ui_history_isolation_test COMMAND ui_history_isolation_test)

  add_executable(ui_candidate_test "${TEST_ROOT}/ui_candidate_test.cpp")
  target_include_directories(ui_candidate_test PRIVATE "${mock_dir}")
  add_test(NAME ui_candidate_test COMMAND ui_candidate_test)

  add_executable(ui_injection_test "${TEST_ROOT}/ui_injection_test.cpp")
  target_include_directories(ui_injection_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME ui_injection_test COMMAND ui_injection_test)

  set(preset_mock_dir "${CMAKE_CURRENT_BINARY_DIR}/preset-mock-include")
  file(MAKE_DIRECTORY "${preset_mock_dir}")
  configure_file("${REPO_ROOT}/src/addons/mfgunlock/fg_preset.hpp" "${preset_mock_dir}/fg_preset.hpp" COPYONLY)
  configure_file("${TEST_ROOT}/fg_preset_mocks/ngx_hook.hpp" "${preset_mock_dir}/ngx_hook.hpp" COPYONLY)
  add_executable(fg_preset_test "${TEST_ROOT}/fg_preset_test.cpp")
  target_include_directories(fg_preset_test PRIVATE "${preset_mock_dir}" "${TEST_ROOT}/provider_mocks"
                                                  "${REPO_ROOT}/src/addons/mfgunlock")
  add_test(NAME fg_preset_test COMMAND fg_preset_test)
endif()

# R6.3.1: the cached-reader contract is pure C++; test it on native Windows too.
add_executable(fg_preset_cached_test "${TEST_ROOT}/fg_preset_cached_test.cpp")
add_test(NAME fg_preset_cached_test COMMAND fg_preset_cached_test)

add_executable(provider_state_test "${TEST_ROOT}/provider_state_test.cpp")
target_include_directories(provider_state_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME provider_state_test COMMAND provider_state_test)

add_executable(validated_warp_test "${TEST_ROOT}/validated_warp_test.cpp")
target_include_directories(validated_warp_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME validated_warp_test COMMAND validated_warp_test)

add_executable(blackwell_temporal_test "${TEST_ROOT}/blackwell_temporal_test.cpp")
target_include_directories(blackwell_temporal_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME blackwell_temporal_test COMMAND blackwell_temporal_test)

# Run the same composition cases against the actual runtime transform, not only
# the inspector's portable copy.
add_executable(temporal_runtime_test "${TEST_ROOT}/temporal_composition_test.cpp")
target_compile_definitions(temporal_runtime_test PRIVATE MFG_TEST_RUNTIME_MIDPOINT=1)
target_include_directories(temporal_runtime_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME temporal_runtime_test COMMAND temporal_runtime_test)

add_executable(endpoint_backend_test "${TEST_ROOT}/endpoint_backend_test.cpp")
target_include_directories(endpoint_backend_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME endpoint_backend_test COMMAND endpoint_backend_test)

add_executable(inspect_provider "${CMAKE_CURRENT_SOURCE_DIR}/tools/inspect_provider.cpp")
add_executable(probe_provider_trial "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_provider_trial.cpp")
target_include_directories(probe_provider_trial PRIVATE "${TEST_ROOT}/provider_mocks")


add_executable(lifecycle_test "${TEST_ROOT}/lifecycle_test.cpp")
target_include_directories(lifecycle_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME lifecycle_test COMMAND lifecycle_test)

add_executable(hook_lifecycle_test "${TEST_ROOT}/hook_lifecycle_test.cpp")
target_include_directories(hook_lifecycle_test PRIVATE "${TEST_ROOT}/hook_mocks" "${TEST_ROOT}/provider_mocks")
add_test(NAME hook_lifecycle_test COMMAND hook_lifecycle_test)

add_executable(mfg_probe_test "${TEST_ROOT}/mfg_probe_test.cpp")
target_include_directories(mfg_probe_test PRIVATE "${TEST_ROOT}/probe_mocks" "${TEST_ROOT}/hook_mocks"
                                                 "${TEST_ROOT}/provider_mocks")
add_test(NAME mfg_probe_test COMMAND mfg_probe_test)

add_executable(diagnostics_timeline_test "${TEST_ROOT}/diagnostics_timeline_test.cpp")
add_test(NAME diagnostics_timeline_test COMMAND diagnostics_timeline_test)

add_executable(diagnostic_bridge_test "${TEST_ROOT}/diagnostic_bridge_test.cpp")
add_test(NAME diagnostic_bridge_test COMMAND diagnostic_bridge_test)

add_executable(integration_contract_test "${TEST_ROOT}/integration_contract_test.cpp")
add_test(NAME integration_contract_test COMMAND integration_contract_test)

add_executable(diagnostics_trace_test "${TEST_ROOT}/diagnostics_trace_test.cpp")
target_include_directories(diagnostics_trace_test PRIVATE "${TEST_ROOT}/diagnostics_mocks")
add_test(NAME diagnostics_trace_test COMMAND diagnostics_trace_test)

add_executable(diagnostics_hud_tag_summary_test "${TEST_ROOT}/diagnostics_hud_tag_summary_test.cpp")
add_test(NAME diagnostics_hud_tag_summary_test COMMAND diagnostics_hud_tag_summary_test)

option(MFG_NATIVE_STACK_TESTS "Exercise production preflight with real Windows/Detours on small stacks" OFF)
set(MFG_DETOURS_SOURCE_DIR "" CACHE PATH "Optional local Microsoft Detours source tree (contains src/detours.h)")
if(MFG_NATIVE_STACK_TESTS)
  if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MFG_NATIVE_STACK_TESTS requires Windows x64 and MSVC")
  endif()
  if(NOT MFG_DETOURS_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(mfg_detours
      GIT_REPOSITORY https://github.com/microsoft/Detours.git
      GIT_TAG v4.0.1
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(mfg_detours)
    set(MFG_DETOURS_SOURCE_DIR "${mfg_detours_SOURCE_DIR}")
  endif()
  set(detours_files detours modules disasm image creatwth disolx86 disolx64 disolia64 disolarm disolarm64)
  set(detours_sources "")
  foreach(file IN LISTS detours_files)
    list(APPEND detours_sources "${MFG_DETOURS_SOURCE_DIR}/src/${file}.cpp")
  endforeach()
  add_library(stack_detours STATIC ${detours_sources})
  target_include_directories(stack_detours PUBLIC "${MFG_DETOURS_SOURCE_DIR}/src")
  target_compile_definitions(stack_detours PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_compile_options(stack_detours PRIVATE /Zc:gotoScope-)
  add_executable(small_stack_test "${TEST_ROOT}/small_stack_test.cpp")
  target_include_directories(small_stack_test PRIVATE "${TEST_ROOT}/native_stack")
  target_compile_definitions(small_stack_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(small_stack_test PRIVATE /W4 /permissive- /EHsc)
  target_link_libraries(small_stack_test PRIVATE stack_detours)
  foreach(kib IN ITEMS 64 128)
    add_test(NAME small_stack_${kib} COMMAND small_stack_test ${kib})
    set_tests_properties(small_stack_${kib} PROPERTIES TIMEOUT 25)
  endforeach()
endif()

add_executable(export_provider_profile "${CMAKE_CURRENT_SOURCE_DIR}/tools/export_provider_profile.cpp")
add_executable(provider_profile_test "${TEST_ROOT}/provider_profile_test.cpp")
add_test(NAME provider_profile_test COMMAND provider_profile_test)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME provider_trial_tool_test
  COMMAND "${Python3_EXECUTABLE}" "${TEST_ROOT}/provider_trial_tool_test.py"
          "$<TARGET_FILE:probe_provider_trial>")

# Use the actual plugin scan preflight from addon.cpp, not a second parser.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_image_seam.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_image_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/addon_image_seam.hpp"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(mapped_image_bounds_test "${TEST_ROOT}/mapped_image_bounds_test.cpp")
target_include_directories(mapped_image_bounds_test PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${TEST_ROOT}/provider_mocks")
add_test(NAME mapped_image_bounds_test COMMAND mapped_image_bounds_test)

# Exact production flip writer/rollback/cache, with injectable OS failures.
# The separate image/corpus probe above continues to use non-writing plan stubs.
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_image_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/addon_flip_transaction_seam.hpp" --transaction
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(flip_transaction_test "${TEST_ROOT}/flip_transaction_test.cpp")
target_include_directories(flip_transaction_test PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${TEST_ROOT}/provider_mocks")
add_test(NAME flip_transaction_test COMMAND flip_transaction_test)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_maintenance_seam.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_maintenance_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/addon_maintenance_seam.hpp"
  COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_mfg_gate_seam.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_addon_mfg_gate_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/addon_mfg_gate_seam.hpp"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(mfg_gate_entry_test "${TEST_ROOT}/mfg_gate_entry_test.cpp")
target_include_directories(mfg_gate_entry_test PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${TEST_ROOT}/provider_mocks")
add_test(NAME mfg_gate_entry_test COMMAND mfg_gate_entry_test)

add_executable(present_maintenance_test "${TEST_ROOT}/present_maintenance_test.cpp")
target_include_directories(present_maintenance_test PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${TEST_ROOT}/hook_mocks" "${TEST_ROOT}/provider_mocks")
add_test(NAME present_maintenance_test COMMAND present_maintenance_test)

# Reuse the exact addon implementation for OTA ceiling replacement and UI rows.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_runtime_compat_seam.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_runtime_compat_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/addon_runtime_compat_seam.hpp"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(runtime_compat_test "${TEST_ROOT}/runtime_compat_test.cpp")
target_include_directories(runtime_compat_test PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${TEST_ROOT}/hook_mocks" "${TEST_ROOT}/provider_mocks")
add_test(NAME runtime_compat_test COMMAND runtime_compat_test)



add_test(NAME provider_qualification_test
  COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/tools/qualify_provider.py" --self-test
          --check-registry "$<TARGET_FILE:export_provider_profile>")
get_property(mfg_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${mfg_tests} PROPERTIES TIMEOUT 60)

# Shared evidence foundation and runtime contract tests.
add_executable(compatibility_foundation_test "${TEST_ROOT}/compatibility_foundation_test.cpp")
add_test(NAME compatibility_foundation_test COMMAND compatibility_foundation_test)
add_executable(compatibility_runtime_test "${TEST_ROOT}/compatibility_runtime_test.cpp")
add_executable(compatibility_contract_probe "${CMAKE_CURRENT_SOURCE_DIR}/tools/compatibility_contract_probe.cpp")
add_test(NAME compatibility_runtime_test COMMAND compatibility_runtime_test)
add_test(NAME compatibility_corpus_unit_test
  COMMAND "${Python3_EXECUTABLE}" "${TEST_ROOT}/compatibility_corpus_test.py")
set_tests_properties(compatibility_foundation_test compatibility_corpus_unit_test PROPERTIES TIMEOUT 60)
add_test(NAME compatibility_hardening_test
  COMMAND "${Python3_EXECUTABLE}" "${TEST_ROOT}/compatibility_hardening_test.py")
set_tests_properties(compatibility_hardening_test PROPERTIES TIMEOUT 120)

# Local corpus is optional. Never download NVIDIA DLLs as part of the build.
set(MFG_COMPAT_CORPUS_ROOT "" CACHE PATH "Local corpus with dlss_g, streamline and meta subdirectories")
if(MFG_COMPAT_CORPUS_ROOT)
  add_test(NAME compatibility_full_corpus_test
    COMMAND "${Python3_EXECUTABLE}" "${TEST_ROOT}/compatibility_corpus_test.py"
            --dlssg-root "${MFG_COMPAT_CORPUS_ROOT}/dlss_g"
            --streamline-root "${MFG_COMPAT_CORPUS_ROOT}/streamline"
            --streamline-meta "${MFG_COMPAT_CORPUS_ROOT}/meta"
            --probe "$<TARGET_FILE:compatibility_foundation_test>")
  set_tests_properties(compatibility_full_corpus_test PROPERTIES TIMEOUT 600)
endif()

add_test(NAME compatibility_local_tool_test
  COMMAND "${Python3_EXECUTABLE}" "${TEST_ROOT}/compatibility_local_tool_test.py"
          --probe "$<TARGET_FILE:compatibility_contract_probe>")
set_tests_properties(compatibility_local_tool_test PROPERTIES TIMEOUT 120)

add_executable(probe_native_cubin "${TEST_ROOT}/../tools/probe_native_cubin.cpp")
target_include_directories(probe_native_cubin PRIVATE "${TEST_ROOT}/provider_mocks")
add_executable(cubin_trial_test "${TEST_ROOT}/cubin_trial_test.cpp")
target_include_directories(cubin_trial_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME cubin_trial_test COMMAND cubin_trial_test)
set_tests_properties(cubin_trial_test PROPERTIES TIMEOUT 60)

# C1 evidence-only provider capability profiler. This target consumes a privately
# mapped PE copy and never loads or calls NVIDIA provider code.
add_executable(probe_provider_capability "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_provider_capability.cpp")
target_include_directories(probe_provider_capability PRIVATE "${TEST_ROOT}/provider_mocks")

add_executable(endpoint_structural_test "${TEST_ROOT}/endpoint_structural_test.cpp")
add_test(NAME endpoint_structural_test COMMAND endpoint_structural_test)
set_tests_properties(endpoint_structural_test PROPERTIES TIMEOUT 60)

# Optional local reference input exercises the extended relocation/endpoint/
# quality transaction, not just the default no-DLL unit-test entry point.
set(MFG_SM75_REFERENCE_PROVIDER "" CACHE FILEPATH "Local reference provider for extended SM75 tests")
if(MFG_SM75_REFERENCE_PROVIDER)
  add_test(NAME endpoint_reference_provider_test
    COMMAND endpoint_backend_test "${MFG_SM75_REFERENCE_PROVIDER}")
  set_tests_properties(endpoint_reference_provider_test PROPERTIES TIMEOUT 180)
endif()

# Bind corpus-probe output to the exact compiled production headers and doubles.
file(GLOB_RECURSE mfg_probe_sources CONFIGURE_DEPENDS
  "${REPO_ROOT}/src/addons/mfgunlock/*.hpp"
  "${TEST_ROOT}/provider_mocks/*.h" "${TEST_ROOT}/provider_mocks/*.hpp")
list(APPEND mfg_probe_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_provider_capability.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/mapped_provider_image.hpp")
list(SORT mfg_probe_sources)
set(mfg_probe_manifest "")
foreach(input IN LISTS mfg_probe_sources)
  file(SHA256 "${input}" digest)
  file(RELATIVE_PATH relative "${REPO_ROOT}" "${input}")
  string(APPEND mfg_probe_manifest "${relative}|${digest}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
endforeach()
string(SHA256 mfg_probe_digest "${mfg_probe_manifest}")
target_compile_definitions(probe_provider_capability PRIVATE MFG_PROBE_SOURCE_ID="${mfg_probe_digest}")
add_test(NAME sm75_tool_test COMMAND "${Python3_EXECUTABLE}"
  "${TEST_ROOT}/sm75_tool_test.py" --probe "$<TARGET_FILE:probe_provider_capability>")
set_tests_properties(sm75_tool_test PROPERTIES TIMEOUT 60)

add_executable(sm75_storage_test "${TEST_ROOT}/sm75_storage_test.cpp")
target_include_directories(sm75_storage_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME sm75_storage_test COMMAND sm75_storage_test)
set_tests_properties(sm75_storage_test PROPERTIES TIMEOUT 60)

# Fused FP16 ReLU lowering and exhaustive post-FMA lane semantics.
add_executable(sm75_relu_test "${TEST_ROOT}/sm75_relu_test.cpp")
add_test(NAME sm75_relu_test COMMAND sm75_relu_test)
set_tests_properties(sm75_relu_test PROPERTIES TIMEOUT 60)

# MG1: exercise production structural host-gate discovery and transaction.
add_executable(mfg_gate_test "${TEST_ROOT}/mfg_gate_test.cpp")
target_include_directories(mfg_gate_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME mfg_gate_test COMMAND mfg_gate_test)
add_executable(mfg_gate_flow_test "${TEST_ROOT}/mfg_gate_flow_test.cpp")
target_include_directories(mfg_gate_flow_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME mfg_gate_flow_test COMMAND mfg_gate_flow_test)
add_executable(probe_mfg_gate "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_mfg_gate.cpp")
target_include_directories(probe_mfg_gate PRIVATE "${TEST_ROOT}/provider_mocks")
# Bind this independent host probe to every source and API double it compiles.
set(mfg_gate_sources ${mfg_probe_sources})
list(REMOVE_ITEM mfg_gate_sources "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_provider_capability.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mapped_provider_image.hpp")
list(APPEND mfg_gate_sources "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_mfg_gate.cpp")
list(SORT mfg_gate_sources)
set(mfg_gate_manifest "")
foreach(input IN LISTS mfg_gate_sources)
  file(SHA256 "${input}" digest)
  file(RELATIVE_PATH relative "${REPO_ROOT}" "${input}")
  string(APPEND mfg_gate_manifest "${relative}|${digest}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
endforeach()
string(SHA256 mfg_gate_digest "${mfg_gate_manifest}")
target_compile_definitions(probe_mfg_gate PRIVATE MFG_GATE_SOURCE_ID="${mfg_gate_digest}")
add_test(NAME mfg_gate_tool_test COMMAND "${Python3_EXECUTABLE}"
  "${TEST_ROOT}/mfg_gate_tool_test.py" --probe "$<TARGET_FILE:probe_mfg_gate>"
  --fixture "$<TARGET_FILE:mfg_gate_test>")
set_tests_properties(mfg_gate_tool_test PROPERTIES TIMEOUT 120)

# F1 evidence collection/projection: no vendor execution and explicit coverage.
add_test(NAME f1_evidence_test COMMAND "${Python3_EXECUTABLE}"
  "${TEST_ROOT}/f1_evidence_test.py")
set_tests_properties(f1_evidence_test PROPERTIES TIMEOUT 60)

# Legacy flat-network contract and the real provider transaction.
add_executable(endpoint_flat_test "${TEST_ROOT}/endpoint_flat_test.cpp")
target_include_directories(endpoint_flat_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME endpoint_flat_test COMMAND endpoint_flat_test)
add_executable(sm75_source_format_test "${TEST_ROOT}/sm75_source_format_test.cpp")
add_test(NAME sm75_source_format_test COMMAND sm75_source_format_test)

# F2: execute the exact production alpha callbacks against explicit API doubles.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_ui_alpha_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${REPO_ROOT}/src/addons/mfgunlock/framecount.hpp")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_ui_alpha_seam.py"
  "${REPO_ROOT}/src/addons/mfgunlock/addon.cpp"
  "${REPO_ROOT}/src/addons/mfgunlock/framecount.hpp"
  "${CMAKE_CURRENT_BINARY_DIR}/ui_alpha_production_seam.hpp"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(ui_alpha_gpu_test "${TEST_ROOT}/ui_alpha_gpu_test.cpp")
target_include_directories(ui_alpha_gpu_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME ui_alpha_gpu_test COMMAND ui_alpha_gpu_test)
set_tests_properties(ui_alpha_gpu_test PROPERTIES TIMEOUT 60)

add_executable(passive_metrics_test "${TEST_ROOT}/passive_metrics_test.cpp")
add_test(NAME passive_metrics_test COMMAND passive_metrics_test)
set_tests_properties(passive_metrics_test PROPERTIES TIMEOUT 60)
if(NOT WIN32)
  add_executable(ui_native_precedence_test "${TEST_ROOT}/ui_native_precedence_test.cpp")
  target_include_directories(ui_native_precedence_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME ui_native_precedence_test COMMAND ui_native_precedence_test)
  set_tests_properties(ui_native_precedence_test PROPERTIES TIMEOUT 60)
endif()

add_executable(vulkan_format_test "${TEST_ROOT}/vulkan_format_test.cpp")
add_test(NAME vulkan_format_test COMMAND vulkan_format_test)

if(NOT WIN32)
  add_executable(vulkan_ui_test "${TEST_ROOT}/vulkan_ui_test.cpp")
  target_include_directories(vulkan_ui_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME vulkan_ui_test COMMAND vulkan_ui_test)
endif()

# Native compile probe uses REAL headers. Enable explicitly; missing SDK files
# are an error, not a silent fall back to the portable API doubles.
option(MFG_NATIVE_VULKAN_ABI_TEST "Compile against real Vulkan/NGX headers" OFF)
if(MFG_NATIVE_VULKAN_ABI_TEST)
  if(NOT WIN32)
    message(FATAL_ERROR "Native ABI probe targets the Windows NGX addon ABI")
  endif()
  set(MFG_VULKAN_INCLUDE "${REPO_ROOT}/external/Vulkan-Headers/include" CACHE PATH "Vulkan headers")
  set(MFG_NGX_INCLUDE "${REPO_ROOT}/external/RenoDX/external/DLSS/include" CACHE PATH "NGX headers")
  foreach(header "${MFG_VULKAN_INCLUDE}/vulkan/vulkan_core.h" "${MFG_NGX_INCLUDE}/nvsdk_ngx_vk.h")
    if(NOT EXISTS "${header}")
      message(FATAL_ERROR "Required native header missing: ${header}")
    endif()
  endforeach()
  add_executable(vulkan_sdk_abi_test "${TEST_ROOT}/vulkan_sdk_abi_test.cpp")
  target_include_directories(vulkan_sdk_abi_test PRIVATE "${MFG_VULKAN_INCLUDE}" "${MFG_NGX_INCLUDE}")
  add_test(NAME vulkan_sdk_abi_test COMMAND vulkan_sdk_abi_test)
endif()
if(NOT WIN32)
  add_executable(vulkan_count_test "${TEST_ROOT}/vulkan_count_test.cpp")
  target_include_directories(vulkan_count_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME vulkan_count_test COMMAND vulkan_count_test)
endif()

add_test(NAME f3_runtime_collector_test COMMAND "${Python3_EXECUTABLE}"
  "${TEST_ROOT}/f3_runtime_collector_test.py")
set_tests_properties(f3_runtime_collector_test PROPERTIES TIMEOUT 60)

# F4: production snapshot/oracle/C1 comparisons and evidence qualification.
# C1 remains test-only until its separate patch is explicitly applied.
add_executable(f4_passive_read_test "${TEST_ROOT}/f4_passive_read_test.cpp")
add_test(NAME f4_passive_read_test COMMAND f4_passive_read_test)
set_tests_properties(f4_passive_read_test PROPERTIES TIMEOUT 60)
add_test(NAME f4_tools_test COMMAND "${Python3_EXECUTABLE}" "${TEST_ROOT}/f4_tools_test.py")
set_tests_properties(f4_tools_test PROPERTIES TIMEOUT 60)

# R6.3: reuse the production private-reader proof in the read-only collector.
if(NOT WIN32)
  add_executable(probe_fg_preset "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_fg_preset.cpp")
  target_include_directories(probe_fg_preset PRIVATE "${preset_mock_dir}" "${TEST_ROOT}/provider_mocks"
                                                     "${REPO_ROOT}/src/addons/mfgunlock")
  set(preset_probe_sources ${mfg_probe_sources})
  list(REMOVE_ITEM preset_probe_sources "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_provider_capability.cpp"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/tools/mapped_provider_image.hpp")
  list(APPEND preset_probe_sources "${CMAKE_CURRENT_SOURCE_DIR}/tools/probe_fg_preset.cpp"
                                   "${TEST_ROOT}/fg_preset_mocks/ngx_hook.hpp")
  list(SORT preset_probe_sources)
  set(preset_probe_manifest "")
  foreach(input IN LISTS preset_probe_sources)
    file(SHA256 "${input}" digest)
    file(RELATIVE_PATH relative "${REPO_ROOT}" "${input}")
    string(APPEND preset_probe_manifest "${relative}|${digest}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  endforeach()
  string(SHA256 preset_probe_digest "${preset_probe_manifest}")
  target_compile_definitions(probe_fg_preset PRIVATE MFG_PRESET_SOURCE_ID="${preset_probe_digest}")
  add_test(NAME preset_evidence_test COMMAND "${Python3_EXECUTABLE}"
    "${TEST_ROOT}/preset_evidence_test.py" --probe "$<TARGET_FILE:probe_fg_preset>")
  set_tests_properties(preset_evidence_test PROPERTIES TIMEOUT 90)
endif()
