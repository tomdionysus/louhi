#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

project(louhi VERSION 0.2.1 LANGUAGES CXX)

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
# On, Dannan and Dagda are fetched at their tags into the build tree, for
# development and CI. Off, the installed packages are found instead
# (libdannan-dev, libdagda-dev), so the binaries link the packaged shared
# libraries: the Debian package's build (docs/packaging.md).
option(LOUHI_FETCH_DEPENDENCIES "Fetch Dannan and Dagda at their tags; off finds the installed packages" ON)

# Where an install puts things; before any target names its install paths.
include(GNUInstallDirs)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)
# louhi-ctl's configuration and credentials files are YAML (the maintainer, 2026-11-20).
# Dannan reads YAML with it too, but links it privately.
find_package(yaml-cpp 0.7 CONFIG REQUIRED)

# Lugh's and Lir's clients are always fetched: neither has a package.
include(FetchContent)

# Dannan, the network layer, at its tag, cloned from the sibling checkout
# into the build tree, so building Louhi never touches ../dannan.
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists. Installed, it is the least patch of the minor accepted
# (Dannan keeps its ABI across a minor's patches).
set(LOUHI_DANNAN_TAG "0.9.2")
if(LOUHI_FETCH_DEPENDENCIES)
  # A string, not a path: CI names a URL, which a PATH cache entry would turn
  # into a directory under the source tree.
  set(LOUHI_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository: a directory or a URL")
  # OVERRIDE_FIND_PACKAGE: Dagda finds Dannan with find_package, which this
  # makes answer with the Dannan fetched here.
  FetchContent_Declare(dannan GIT_REPOSITORY "${LOUHI_DANNAN_REPOSITORY}" GIT_TAG "${LOUHI_DANNAN_TAG}" OVERRIDE_FIND_PACKAGE)
  set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(dannan)
else()
  find_package(dannan ${LOUHI_DANNAN_TAG} CONFIG REQUIRED)
endif()

# Dagda, the storage layer, where Louhi's records live (design, "Records"),
# at its tag, fetched like Dannan (the maintainer, 2026-11-20). Its tools are not
# built: EXCLUDE_FROM_ALL builds only what Louhi links. Off builds Louhi
# without the Dagda store, its records then kept in memory only.
option(LOUHI_BUILD_DAGDA "Build the store on Dagda's head spaces" ON)
if(LOUHI_BUILD_DAGDA)
  set(LOUHI_DAGDA_TAG "0.3.2")
  if(LOUHI_FETCH_DEPENDENCIES)
    set(LOUHI_DAGDA_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dagda" CACHE STRING "Dagda's repository: a directory or a URL")
    FetchContent_Declare(dagda GIT_REPOSITORY "${LOUHI_DAGDA_REPOSITORY}" GIT_TAG "${LOUHI_DAGDA_TAG}" EXCLUDE_FROM_ALL)
    set(DAGDA_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(dagda)
  else()
    # Each Dagda release is its own SONAME and package for now, so the tag.
    find_package(dagda ${LOUHI_DAGDA_TAG} CONFIG REQUIRED)
  endif()
endif()

# Lugh's client, for the Lugh plugin (sign-in to the API), at a commit of its
# develop on Dannan's minor; it takes the Dannan above rather than fetching
# its own. Off builds Louhi without it, and louhi-node then serves no API:
# there is no anonymous mode (the maintainer, 2026-11-15).
option(LOUHI_BUILD_LUGH "Build the Lugh plugin and the API's sign-in" ON)
if(LOUHI_BUILD_LUGH)
  set(LOUHI_LUGH_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lugh" CACHE STRING "Lugh's repository: a directory or a URL")
  # Lugh's release, on the same Dannan (0.9.2).
  set(LOUHI_LUGH_REF "0.2.0")
  FetchContent_Declare(lugh GIT_REPOSITORY "${LOUHI_LUGH_REPOSITORY}" GIT_TAG "${LOUHI_LUGH_REF}")
  set(LUGH_BUILD_NODE OFF CACHE BOOL "" FORCE)
  # Lugh's warnings are errors as Louhi's are: in the release and clang gates,
  # not under a sanitizer, where g++ warns inside Asio (Lugh's own CI holds
  # its warning gate).
  set(LUGH_WARNINGS_AS_ERRORS ${LOUHI_WARNINGS_AS_ERRORS} CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lugh)
endif()

# Lir's client, for publishing Louhi's events (design, "Events"), at a commit
# of its develop on Louhi's Dannan, which it takes rather than fetching its
# own. Only lir::mqtt and lir::client are built. Off by default, so a release
# carries no events: Lir's releases ship no client until liblir-client0.1, and
# this pin is the withdrawn one (2026-11-20). On builds and tests the events
# code against it (tools/ci.sh events).
option(LOUHI_BUILD_LIR "Build the events publisher on Lir's client (unreleased; see above)" OFF)
if(LOUHI_BUILD_LIR)
  set(LOUHI_LIR_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lir" CACHE STRING "Lir's repository: a directory or a URL")
  set(LOUHI_LIR_REF "e40dd2e4fda06b24546b3f68df11fcc9344e7cab")
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

# One node of Louhi: a Dannan node hosting the agent. Its configuration file
# is found under the install's sysconfdir when none is named: /etc/louhi for
# the package, /usr/local/etc/louhi for a source install.
add_executable(louhi-node tools/louhi_node.cpp)
target_link_libraries(louhi-node PRIVATE louhi louhi_warnings)
target_compile_definitions(louhi-node PRIVATE LOUHI_DEFAULT_CONFIG="${CMAKE_INSTALL_FULL_SYSCONFDIR}/louhi/config.yaml")
install(TARGETS louhi-node RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/man/louhi-node.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# The API from a terminal; it signs in through Lugh, so it needs the plugin.
if(LOUHI_BUILD_LUGH)
  add_executable(louhi-ctl tools/louhi_ctl.cpp)
  target_link_libraries(louhi-ctl PRIVATE louhi louhi_warnings)
  install(TARGETS louhi-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES docs/man/louhi-ctl.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

# The example configuration the README documents, and the node's unit. The
# generated files name the same install directories as the binaries, so they
# agree on every prefix. An existing configuration is never overwritten: it
# is the site's. The Debian package ships it as a conffile instead.
configure_file(config/config.yaml.in config.yaml @ONLY)
install(CODE "
  set(louhi_config \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_SYSCONFDIR}/louhi/config.yaml\")
  if(EXISTS \"\${louhi_config}\")
    message(STATUS \"Keeping existing configuration: \${louhi_config}\")
  else()
    # file(INSTALL) applies DESTDIR itself.
    file(INSTALL \"${CMAKE_CURRENT_BINARY_DIR}/config.yaml\" DESTINATION \"${CMAKE_INSTALL_FULL_SYSCONFDIR}/louhi\")
  endif()")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  configure_file(packaging/louhi.service.in louhi.service @ONLY)
  # systemd reads units from <prefix>/lib/systemd/system, never lib64 or a multiarch libdir.
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/louhi.service DESTINATION lib/systemd/system)
endif()

if(LOUHI_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(louhi/test)
endif()

if(LOUHI_BUILD_FUZZING)
  add_subdirectory(louhi/fuzz)
endif()
