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
  admit/agreement.cpp
  admit/registers.cpp
  agent/agent.cpp
  agent/specs.cpp
  agent/store_bindings.cpp
  api/api.cpp
  api/auth.cpp
  api/client.cpp
  api/http.cpp
  api/json.cpp
  events/events.cpp
  model/codec.cpp
  model/ids.cpp
  model/image.cpp
  model/keys.cpp
  model/names.cpp
  model/network.cpp
  model/records.cpp
  net/command.cpp
  net/firewall.cpp
  net/host.cpp
  node/status.cpp
  runtime/docker/api.cpp
  runtime/docker/docker_driver.cpp
  runtime/docker/local_http.cpp
  runtime/driver.cpp
  runtime/labels.cpp
  runtime/registries.cpp
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
# Boost.JSON's definitions are compiled once in a process: lugh_client's when
# the Lugh plugin is built (it compiles them itself), Louhi's otherwise.
if(LOUHI_BUILD_LUGH)
  target_sources(louhi PRIVATE auth/lugh.cpp auth/service_token.cpp ctl/ctl.cpp)
  target_link_libraries(louhi PUBLIC lugh::client)
  target_compile_definitions(louhi PUBLIC LOUHI_WITH_LUGH=1)
else()
  target_sources(louhi PRIVATE runtime/docker/boost_json.cpp)
endif()
if(LOUHI_BUILD_DAGDA)
  target_link_libraries(louhi PUBLIC dagda::dagda)
  target_compile_definitions(louhi PUBLIC LOUHI_WITH_DAGDA=1)
endif()
if(LOUHI_BUILD_LIR)
  target_sources(louhi PRIVATE events/lir.cpp)
  target_link_libraries(louhi PUBLIC lir::client)
  target_compile_definitions(louhi PUBLIC LOUHI_WITH_LIR=1)
endif()
set_target_properties(louhi PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
