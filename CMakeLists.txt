cmake_minimum_required(VERSION 3.8)

project(Measure C CXX)

include(./common.cmake)

find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
  message(FATAL_ERROR "SQLite3 development files are required")
endif()

# Proto file
get_filename_component(m_proto "./measure.proto" ABSOLUTE)
get_filename_component(m_proto_path "${m_proto}" PATH)

# Generated sources
set(m_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/measure.pb.cc")
set(m_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/measure.pb.h")
set(m_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/measure.grpc.pb.cc")
set(m_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/measure.grpc.pb.h")
add_custom_command(
      OUTPUT "${m_proto_srcs}" "${m_proto_hdrs}" "${m_grpc_srcs}" "${m_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${m_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${m_proto}"
      DEPENDS "${m_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# m_grpc_proto
add_library(m_grpc_proto
  ${m_grpc_srcs}
  ${m_grpc_hdrs}
  ${m_proto_srcs}
  ${m_proto_hdrs})
target_link_libraries(m_grpc_proto
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

add_library(measurement_utils src/measurement_utils.cc)
target_compile_features(measurement_utils PUBLIC cxx_std_17)
target_include_directories(measurement_utils PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(measure_service src/measure_service.cc src/event_store.cc)
target_compile_features(measure_service PUBLIC cxx_std_17)
target_include_directories(measure_service
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PRIVATE "${SQLITE3_INCLUDE_DIR}")
target_link_libraries(measure_service
  m_grpc_proto
  measurement_utils
  "${SQLITE3_LIBRARY}")

add_library(measure_client_runtime src/measure_client_runtime.cc)
target_compile_features(measure_client_runtime PUBLIC cxx_std_17)
target_include_directories(measure_client_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(measure_client_runtime PUBLIC m_grpc_proto)

# Targets measure_[async_](client|server)
foreach(_target
  measure_client measure_server)
  add_executable(${_target} "src/${_target}.cc")
  target_compile_features(${_target} PRIVATE cxx_std_17)
  target_link_libraries(${_target}
    m_grpc_proto
    absl::flags
    absl::flags_parse
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
endforeach()
target_sources(measure_server PRIVATE src/dashboard_server.cc)
target_link_libraries(measure_server measurement_utils measure_service)
target_link_libraries(measure_client measure_client_runtime)

add_executable(measure_healthcheck src/measure_healthcheck.cc)
target_compile_features(measure_healthcheck PRIVATE cxx_std_17)

option(BUILD_TESTING "Build Measure tests" OFF)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1)
  FetchContent_MakeAvailable(Catch2)
  enable_testing()
  add_subdirectory(tests)
endif()
