#ifndef MEASURE_EVENT_STORE_H
#define MEASURE_EVENT_STORE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace measure {

struct StoredMeasurement {
  std::string client_id;
  int point;
  int64_t timestamp_unix_ms;
};

struct StoredEvent {
  int64_t id;
  std::string event_type;
  int64_t timestamp_unix_ms;
  std::string client_id;
  std::optional<int> point;
  std::optional<int> threshold;
  std::string mode;
  std::string payload_json;
};

struct MeasurementTrend {
  std::string client_id;
  int64_t bucket_start_unix_ms;
  int count;
  double average;
  int minimum;
  int maximum;
};

class EventStore {
 public:
  explicit EventStore(const std::string& path, int retention_days = 0);
  ~EventStore();

  EventStore(const EventStore&) = delete;
  EventStore& operator=(const EventStore&) = delete;

  void RecordMeasurement(const StoredMeasurement& measurement);
  void RecordEvent(const std::string& event_type, int64_t timestamp_unix_ms,
                   const std::string& client_id, const std::string& payload_json,
                   std::optional<int> point = std::nullopt,
                   std::optional<int> threshold = std::nullopt,
                   const std::string& mode = "");
  std::vector<StoredMeasurement> GetMeasurements(size_t limit) const;
  std::vector<StoredEvent> GetEvents(size_t limit, int64_t since_unix_ms = 0,
                                     int64_t until_unix_ms = 0,
                                     const std::string& client_id = "") const;
  std::vector<MeasurementTrend> GetTrends(
      int64_t bucket_ms, int64_t since_unix_ms = 0,
      int64_t until_unix_ms = 0,
      const std::string& client_id = "") const;
  int GetLatestThreshold(int fallback) const;

 private:
  void PruneExpiredEvents();

  sqlite3* database_ = nullptr;
  int retention_days_;
  mutable std::mutex mutex_;
};

}

#endif
