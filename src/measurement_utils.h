#ifndef MEASURE_MEASUREMENT_UTILS_H
#define MEASURE_MEASUREMENT_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

#include "event_store.h"

namespace measure {

std::string JsonEscape(const std::string& value);
std::string BuildMeasurementsJson(
    const std::vector<StoredMeasurement>& measurements);

}

#endif
