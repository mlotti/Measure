add_executable(measurement_utils_tests measurement_utils_tests.cc)
target_link_libraries(measurement_utils_tests
  PRIVATE
    Catch2::Catch2WithMain
    measurement_utils)

add_test(NAME measurement_utils_tests COMMAND measurement_utils_tests)
