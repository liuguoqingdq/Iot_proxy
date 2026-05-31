cmake_minimum_required(VERSION 3.12)

project(iota_proxy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

find_path(RDKAFKA_INCLUDE_DIR librdkafka/rdkafka.h)
find_library(RDKAFKA_LIBRARY rdkafka)
if(NOT RDKAFKA_INCLUDE_DIR OR NOT RDKAFKA_LIBRARY)
  message(FATAL_ERROR "librdkafka is required. Install it with: sudo apt install -y librdkafka-dev")
endif()

add_subdirectory(
  ${CMAKE_CURRENT_LIST_DIR}/../myring
  ${CMAKE_CURRENT_BINARY_DIR}/myring-build
)

add_executable(iota_proxy
  src/app/main.cpp
  src/admission/AdmissionControl.cpp
  src/business/EdgeDataMessage.cpp
  src/business/EdgeDataPipeline.cpp
  src/business/KafkaSink.cpp
  src/business/NodeIdentity.cpp
  src/business/PassThroughBusiness.cpp
  src/business/RedisStateCache.cpp
  src/common/Endpoint.cpp
  src/discovery/DiscoveryService.cpp
  src/discovery/RouteManager.cpp
  src/frame/EdgeDataFrame.cpp
  src/frame/TunnelFrame.cpp
  src/kcp/KcpTunnel.cpp
  src/network/TcpEgress.cpp
  src/network/TcpIngress.cpp
  src/ProxyRuntime.cpp
)

target_include_directories(iota_proxy
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${RDKAFKA_INCLUDE_DIR}
)

target_link_libraries(iota_proxy
  PRIVATE
    myring
    OpenSSL::Crypto
    ${RDKAFKA_LIBRARY}
)

target_compile_definitions(iota_proxy
  PRIVATE
    IOTA_PROXY_DEFAULT_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config"
)

target_compile_options(iota_proxy
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
