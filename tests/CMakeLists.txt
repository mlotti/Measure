add_executable(measurement_utils_tests measurement_utils_tests.cc)
target_link_libraries(measurement_utils_tests
  PRIVATE
    Catch2::Catch2WithMain
    measurement_utils)

add_test(NAME measurement_utils_tests COMMAND measurement_utils_tests)

add_executable(measure_service_tests measure_service_tests.cc)
target_compile_definitions(measure_service_tests
  PRIVATE MEASURE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
target_link_libraries(measure_service_tests
  PRIVATE
    Catch2::Catch2WithMain
    measure_service
    measure_client_runtime
    m_grpc_proto)

add_test(NAME measure_service_tests COMMAND measure_service_tests)

add_executable(event_store_tests event_store_tests.cc)
target_link_libraries(event_store_tests
  PRIVATE
    Catch2::Catch2WithMain
    measure_service)

add_test(NAME event_store_tests COMMAND event_store_tests)

add_executable(dashboard_server_tests
  dashboard_server_tests.cc
  ../src/dashboard_server.cc)
target_link_libraries(dashboard_server_tests
  PRIVATE
    Catch2::Catch2WithMain
    measure_service)

add_test(NAME dashboard_server_tests COMMAND dashboard_server_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME healthcheck_tests
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/healthcheck_tests.py"
    "$<TARGET_FILE:measure_healthcheck>")
set_tests_properties(healthcheck_tests PROPERTIES TIMEOUT 30)
