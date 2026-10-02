#include "measurement_utils.h"

namespace measure {

std::string JsonEscape(const std::string& value) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  std::string escaped;
  escaped.reserve(value.size());
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (c < 0x20) {
          escaped += "\\u00";
          escaped += kHexDigits[c >> 4];
          escaped += kHexDigits[c & 0x0f];
        } else {
          escaped += static_cast<char>(c);
        }
        break;
    }
  }
  return escaped;
}

std::string BuildMeasurementsJson(
    const std::vector<StoredMeasurement>& measurements) {
  std::string json = "{\"measurements\":[";
  for (size_t i = 0; i < measurements.size(); ++i) {
    if (i > 0) json += ",";
    json += "{\"client_id\":\"" + JsonEscape(measurements[i].client_id) +
            "\",\"point\":" + std::to_string(measurements[i].point) +
            ",\"timestamp_unix_ms\":" +
            std::to_string(measurements[i].timestamp_unix_ms) + "}";
  }
  json += "]}";
  return json;
}

}
