cmake_minimum_required(VERSION 3.24)
project(mfgampereunlock_validation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tests")

foreach(test architecture_test early_load_test ampere_ptx_test temporal_composition_test ampere_caps_test)
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
  foreach(name architecture.hpp ampere_caps.hpp ampere_ngx.hpp ampere_policy.hpp framecount.hpp)
    configure_file("${REPO_ROOT}/src/addons/mfgunlock/${name}" "${mock_dir}/${name}" COPYONLY)
  endforeach()
  add_executable(host_wrappers_test "${TEST_ROOT}/host_wrappers_test.cpp")
  target_include_directories(host_wrappers_test PRIVATE "${mock_dir}")
  add_test(NAME host_wrappers_test COMMAND host_wrappers_test)

  add_executable(framecount_profile_test "${TEST_ROOT}/framecount_profile_test.cpp")
  target_include_directories(framecount_profile_test PRIVATE "${mock_dir}" "${TEST_ROOT}/framecount_mocks")
  add_test(NAME framecount_profile_test COMMAND framecount_profile_test)
endif()

add_executable(provider_state_test "${TEST_ROOT}/provider_state_test.cpp")
target_include_directories(provider_state_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME provider_state_test COMMAND provider_state_test)

# Run the same composition cases against the actual runtime transform, not only
# the inspector's portable copy.
add_executable(temporal_runtime_test "${TEST_ROOT}/temporal_composition_test.cpp")
target_compile_definitions(temporal_runtime_test PRIVATE MFG_TEST_RUNTIME_MIDPOINT=1)
target_include_directories(temporal_runtime_test PRIVATE "${TEST_ROOT}/provider_mocks")
add_test(NAME temporal_runtime_test COMMAND temporal_runtime_test)

add_executable(inspect_provider "${CMAKE_CURRENT_SOURCE_DIR}/tools/inspect_provider.cpp")
