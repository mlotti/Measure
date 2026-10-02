add_executable(measurement_utils_tests measurement_utils_tests.cc)
target_link_libraries(measurement_utils_tests
  PRIVATE
    Catch2::Catch2WithMain
    measurement_utils)

add_test(NAME measurement_utils_tests COMMAND measurement_utils_tests)

add_executable(measure_service_tests measure_service_tests.cc)
target_link_libraries(measure_service_tests
  PRIVATE
    Catch2::Catch2WithMain
    measure_service
    m_grpc_proto)

add_test(NAME measure_service_tests COMMAND measure_service_tests)
