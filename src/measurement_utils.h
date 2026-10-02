#ifndef MEASURE_MEASUREMENT_UTILS_H
#define MEASURE_MEASUREMENT_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

namespace measure {

struct StoredMeasurement {
  std::string client_id;
  int point;
  int64_t timestamp_unix_ms;
};

std::string JsonEscape(const std::string& value);
std::string BuildMeasurementsJson(
    const std::vector<StoredMeasurement>& measurements);

}

#endif
