#include "measurement_utils.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("JSON strings escape quotes, slashes, and control characters") {
  REQUIRE(measure::JsonEscape("sensor\"\\\n\t") ==
          "sensor\\\"\\\\\\n\\t");
  REQUIRE(measure::JsonEscape(std::string(1, '\x01')) == "\\u0001");
}

TEST_CASE("measurement JSON serializes empty histories") {
  REQUIRE(measure::BuildMeasurementsJson({}) == "{\"measurements\":[]}");
}

TEST_CASE("measurement JSON serializes measurement fields") {
  const std::vector<measure::StoredMeasurement> measurements = {
      {"sensor-1", 42, 123456}};
  REQUIRE(measure::BuildMeasurementsJson(measurements) ==
          "{\"measurements\":[{\"client_id\":\"sensor-1\",\"point\":42,"
          "\"timestamp_unix_ms\":123456}]}");
}

TEST_CASE("measurement JSON escapes client identifiers") {
  const std::vector<measure::StoredMeasurement> measurements = {
      {"sensor\"1", -3, 99}};
  REQUIRE(measure::BuildMeasurementsJson(measurements) ==
          "{\"measurements\":[{\"client_id\":\"sensor\\\"1\",\"point\":-3,"
          "\"timestamp_unix_ms\":99}]}");
}
