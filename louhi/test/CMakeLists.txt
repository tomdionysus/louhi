#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(louhi_tests
  admit_agreement_test.cpp
  admit_registers_test.cpp
  agent_docker_test.cpp
  agent_specs_test.cpp
  agent_store_bindings_test.cpp
  agent_test.cpp
  api_auth_test.cpp
  api_http_test.cpp
  api_json_test.cpp
  api_openapi_test.cpp
  api_test.cpp
  events_test.cpp
  model_codec_test.cpp
  model_image_test.cpp
  model_keys_test.cpp
  model_names_test.cpp
  model_network_test.cpp
  model_records_test.cpp
  net_command_test.cpp
  net_firewall_test.cpp
  net_host_test.cpp
  node_status_test.cpp
  runtime_docker_api_test.cpp
  runtime_docker_driver_test.cpp
  runtime_driver_test.cpp
  runtime_host_files_test.cpp
  runtime_local_http_test.cpp
  runtime_registries_test.cpp
  store_test.cpp
  version_test.cpp
  fuzz_smoke_test.cpp
  # The fuzz targets, run here through Dannan's harness on every gate.
  ../fuzz/record_fuzz.cpp
  ../fuzz/image_reference_fuzz.cpp
  ../fuzz/docker_reply_fuzz.cpp
  ../fuzz/api_body_fuzz.cpp
)
if(LOUHI_BUILD_LUGH)
  target_sources(louhi_tests PRIVATE auth_lugh_test.cpp ctl_test.cpp)
  # ctl_test.cpp reads the YAML louhi-ctl writes.
  target_link_libraries(louhi_tests PRIVATE yaml-cpp::yaml-cpp)
endif()
if(LOUHI_BUILD_LIR)
  target_sources(louhi_tests PRIVATE events_lir_test.cpp)
endif()
# Dannan's testing library: the simulation's checkers and the fuzz harness,
# named dannan::testing fetched or installed (Dannan 0.10.0).
target_link_libraries(louhi_tests PRIVATE louhi dannan::testing GTest::gtest_main louhi_warnings)
# Dannan's fixtures (temp_dir.h) write only inside Louhi's build tree.
target_compile_definitions(louhi_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")
