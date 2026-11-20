#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

project(louhi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/louhi_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOUHI_BUILD_TESTING "Build louhi_tests (GoogleTest)" OFF)
option(LOUHI_BUILD_FUZZING "Build the libFuzzer targets in louhi/fuzz; needs a clang that ships libFuzzer" OFF)
option(LOUHI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# Dannan, the network layer, at its tag, cloned from the sibling checkout
# into the build tree, so building Louhi never touches ../dannan.
# A string, not a path: CI names a URL, which a PATH cache entry would turn
# into a directory under the source tree.
set(LOUHI_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository: a directory or a URL")
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(LOUHI_DANNAN_TAG "0.9.2")
include(FetchContent)
FetchContent_Declare(dannan GIT_REPOSITORY "${LOUHI_DANNAN_REPOSITORY}" GIT_TAG "${LOUHI_DANNAN_TAG}")
set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dannan)

# Lugh's client, for the Lugh plugin (sign-in to the API), at a commit of its
# develop on Dannan's minor; it takes the Dannan above rather than fetching
# its own. Off builds Louhi without it, and louhi-node then serves no API:
# there is no anonymous mode (the maintainer, 2026-11-15).
option(LOUHI_BUILD_LUGH "Build the Lugh plugin and the API's sign-in" ON)
if(LOUHI_BUILD_LUGH)
  set(LOUHI_LUGH_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lugh" CACHE STRING "Lugh's repository: a directory or a URL")
  # The commit Lir builds its Lugh plugin at, on the same Dannan.
  set(LOUHI_LUGH_REF "dbb0573420952336ede86d8db3e86640e48dddb3")
  FetchContent_Declare(lugh GIT_REPOSITORY "${LOUHI_LUGH_REPOSITORY}" GIT_TAG "${LOUHI_LUGH_REF}")
  set(LUGH_BUILD_NODE OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lugh)
endif()

# Lir's client, for publishing Louhi's events (design, "Events"), at a commit
# of its develop on Louhi's Dannan, which it takes rather than fetching its
# own. Only lir::mqtt and lir::client are built. Off: no events.
option(LOUHI_BUILD_LIR "Build the events publisher on Lir's client" ON)
if(LOUHI_BUILD_LIR)
  set(LOUHI_LIR_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lir" CACHE STRING "Lir's repository: a directory or a URL")
  set(LOUHI_LIR_REF "6268fefcba760402220acc19bbe41f610b65ec9b")
  FetchContent_Declare(lir GIT_REPOSITORY "${LOUHI_LIR_REPOSITORY}" GIT_TAG "${LOUHI_LIR_REF}")
  set(LIR_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lir)
endif()

add_library(louhi_warnings INTERFACE)
target_compile_options(louhi_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(louhi_warnings INTERFACE -Wthread-safety)
endif()
if(LOUHI_WARNINGS_AS_ERRORS)
  target_compile_options(louhi_warnings INTERFACE -Werror)
endif()

configure_file(louhi/include/louhi/version.h.in ${PROJECT_BINARY_DIR}/generated/louhi/version.h @ONLY)

add_subdirectory(louhi)

# One node of Louhi: a Dannan node hosting the agent.
add_executable(louhi-node tools/louhi_node.cpp)
target_link_libraries(louhi-node PRIVATE louhi louhi_warnings)

# The API from a terminal; it signs in through Lugh, so it needs the plugin.
if(LOUHI_BUILD_LUGH)
  add_executable(louhi-ctl tools/louhi_ctl.cpp)
  target_link_libraries(louhi-ctl PRIVATE louhi louhi_warnings)
endif()

if(LOUHI_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(louhi/test)
endif()

if(LOUHI_BUILD_FUZZING)
  add_subdirectory(louhi/fuzz)
endif()
