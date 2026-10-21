#ifndef MEASURE_DEVICE_IDENTITY_H
#define MEASURE_DEVICE_IDENTITY_H

#include <string>

namespace measure {

inline bool IsDeviceIdentityForClient(const std::string& identity,
                                      const std::string& client_id) {
  return !client_id.empty() && identity == "device:" + client_id;
}

inline bool IsOperatorIdentity(const std::string& identity) {
  return identity.compare(0, 9, "operator:") == 0 && identity.size() > 9;
}

}

#endif
