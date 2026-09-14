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
               framecount.hpp ui_candidate.hpp)
    configure_file("${REPO_ROOT}/src/addons/mfgunlock/${name}" "${mock_dir}/${name}" COPYONLY)
  endforeach()
  configure_file("${REPO_ROOT}/src/addons/mfgunlock/quality_guard.hpp"
                 "${mock_dir}/quality_guard.hpp" COPYONLY)
  add_executable(host_wrappers_test "${TEST_ROOT}/host_wrappers_test.cpp")
  target_include_directories(host_wrappers_test PRIVATE "${mock_dir}")
  add_test(NAME host_wrappers_test COMMAND host_wrappers_test)

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

# Run the same composition cases against the actual runtime transform, not only
# the inspector's portable copy.
add_executable(temporal_runtime_test "${TEST_ROOT}/temporal_composition_test.cpp")
target_compile_definitions(temporal_runtime_test PRIVATE MFG_TEST_RUNTIME_MIDPOINT=1)
target_include_directories(temporal_runtime_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME temporal_runtime_test COMMAND temporal_runtime_test)

add_executable(inspect_provider "${CMAKE_CURRENT_SOURCE_DIR}/tools/inspect_provider.cpp")
