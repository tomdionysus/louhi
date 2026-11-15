#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(louhi_tests
  agent_docker_test.cpp
  agent_specs_test.cpp
  agent_test.cpp
  model_codec_test.cpp
  model_image_test.cpp
  model_keys_test.cpp
  model_names_test.cpp
  model_records_test.cpp
  runtime_docker_api_test.cpp
  runtime_docker_driver_test.cpp
  runtime_local_http_test.cpp
  version_test.cpp
)
target_link_libraries(louhi_tests PRIVATE louhi GTest::gtest_main louhi_warnings)
# Dannan's fixtures (temp_dir.h) write only inside Louhi's build tree.
target_compile_definitions(louhi_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")
