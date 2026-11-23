#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One target per parser of untrusted bytes, registered with Dannan's fuzz
# harness (dannan/fuzz/fuzz.h), built as a libFuzzer binary each (clang).
# louhi_tests runs the same targets through dannan::fuzz::smoke on every gate
# (louhi/test/CMakeLists.txt).
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "LOUHI_BUILD_FUZZING needs clang")
endif()
foreach(target record image_reference docker_reply api_body)
  add_executable(${target}_fuzz ${target}_fuzz.cpp driver.cpp)
  target_compile_definitions(${target}_fuzz PRIVATE LOUHI_LIBFUZZER)
  target_compile_options(${target}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(${target}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(${target}_fuzz PRIVATE louhi dannan::testing louhi_warnings)
endforeach()
