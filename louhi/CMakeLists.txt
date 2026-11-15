#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The compute layer. Public headers are louhi/include/louhi/*.h, included as
# "louhi/x.h"; everything else is included from the repository root as
# "louhi/<subsystem>/x.h".
add_library(louhi SHARED
  agent/agent.cpp
  agent/specs.cpp
  agent/store_bindings.cpp
  model/codec.cpp
  model/ids.cpp
  model/image.cpp
  model/keys.cpp
  model/names.cpp
  model/records.cpp
  node/status.cpp
  runtime/docker/api.cpp
  runtime/docker/boost_json.cpp
  runtime/docker/docker_driver.cpp
  runtime/docker/local_http.cpp
  runtime/driver.cpp
  runtime/labels.cpp
  store/memory_store.cpp
  version.cpp
)
target_include_directories(louhi
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(louhi
  PUBLIC dannan::dannan Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE louhi_warnings
)
set_target_properties(louhi PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
