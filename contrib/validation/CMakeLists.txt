cmake_minimum_required(VERSION 3.24)
project(mfgampereunlock_validation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tests")

foreach(test architecture_test early_load_test ptx_retarget_test temporal_composition_test capability_policy_test)
  add_executable(${test} "${TEST_ROOT}/${test}.cpp")
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# The host-wrapper test compiles the real production headers against explicit
# API doubles. It is intentionally non-Windows: the real addon build uses the
# NVIDIA/Windows/ReShade headers instead of these mocks.
if(NOT WIN32)
  set(mock_dir "${CMAKE_CURRENT_BINARY_DIR}/host-mock-include")
  file(MAKE_DIRECTORY "${mock_dir}")
  file(GLOB mock_headers "${TEST_ROOT}/host_mocks/*.h" "${TEST_ROOT}/host_mocks/*.hpp")
  foreach(header IN LISTS mock_headers)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "${mock_dir}/${name}" COPYONLY)
  endforeach()
  foreach(name architecture.hpp streamline_bridge.hpp ngx_bridge.hpp capability_policy.hpp
               framecount.hpp ui_candidate.hpp count_observer.hpp diagnostic_bridge.hpp quality_config.hpp)
    configure_file("${REPO_ROOT}/src/addons/mfgunlock/${name}" "${mock_dir}/${name}" COPYONLY)
  endforeach()
  configure_file("${REPO_ROOT}/src/addons/mfgunlock/quality_guard.hpp"
                 "${mock_dir}/quality_guard.hpp" COPYONLY)
  add_executable(host_wrappers_test "${TEST_ROOT}/host_wrappers_test.cpp")
  target_include_directories(host_wrappers_test PRIVATE "${mock_dir}")
  add_test(NAME host_wrappers_test COMMAND host_wrappers_test)

  add_executable(runtime_settings_test "${TEST_ROOT}/runtime_settings_test.cpp")
  target_include_directories(runtime_settings_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME runtime_settings_test COMMAND runtime_settings_test)

  add_executable(framecount_profile_test "${TEST_ROOT}/framecount_profile_test.cpp")
  target_include_directories(framecount_profile_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME framecount_profile_test COMMAND framecount_profile_test)

  add_executable(quality_guard_test "${TEST_ROOT}/quality_guard_test.cpp")
  target_include_directories(quality_guard_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME quality_guard_test COMMAND quality_guard_test)

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

add_executable(inspect_provider "${CMAKE_CURRENT_SOURCE_DIR}/tools/inspect_provider.cpp")

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

add_executable(diagnostics_trace_test "${TEST_ROOT}/diagnostics_trace_test.cpp")
target_include_directories(diagnostics_trace_test PRIVATE "${TEST_ROOT}/diagnostics_mocks")
add_test(NAME diagnostics_trace_test COMMAND diagnostics_trace_test)

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
