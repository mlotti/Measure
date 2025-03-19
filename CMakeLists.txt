cmake_minimum_required(VERSION 3.8)

project(Measure C CXX)

include(./common.cmake)

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
  absl::check
  ${_REFLECTION}
  ${_GRPC_GRPCPP}
  ${_PROTOBUF_LIBPROTOBUF})

# Targets measure_[async_](client|server)
foreach(_target
  measure_client measure_server)
  add_executable(${_target} "${_target}.cc")
  target_link_libraries(${_target}
    m_grpc_proto
    absl::check
    absl::flags
    absl::flags_parse
    absl::log
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
endforeach()
