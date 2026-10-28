#ifndef MEASURE_EVENT_STORE_H
#define MEASURE_EVENT_STORE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace measure {

struct StoredMeasurement {
  std::string client_id;
  int point;
  int64_t timestamp_unix_ms;
  std::string measurement_id;
};

struct DesiredCommand {
  int64_t revision;
  int threshold;
  int64_t expires_unix_ms;
};

struct CommandDelivery {
  std::string client_id;
  int64_t desired_revision;
  int64_t acknowledged_revision;
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

  bool RecordMeasurement(const StoredMeasurement& measurement);
  void RecordEvent(const std::string& event_type, int64_t timestamp_unix_ms,
                   const std::string& client_id, const std::string& payload_json,
                   std::optional<int> point = std::nullopt,
                   std::optional<int> threshold = std::nullopt,
                   const std::string& mode = "");
  std::vector<StoredMeasurement> GetMeasurements(size_t limit) const;
  std::vector<StoredMeasurement> GetLatestMeasurements() const;
  std::vector<StoredEvent> GetEvents(size_t limit, int64_t since_unix_ms = 0,
                                     int64_t until_unix_ms = 0,
                                     const std::string& client_id = "") const;
  std::vector<MeasurementTrend> GetTrends(
      int64_t bucket_ms, int64_t since_unix_ms = 0,
      int64_t until_unix_ms = 0,
      const std::string& client_id = "") const;
  int GetLatestThreshold(int fallback) const;
  DesiredCommand GetDesiredCommand(const std::string& client_id) const;
  void RegisterDevice(const std::string& client_id);
  void SetDesiredThreshold(int threshold, const std::string& actor);
  void SetDesiredCalibration(const std::vector<std::string>& clients,
                             int64_t expires_unix_ms, const std::string& actor);
  bool ExpireCalibrations(int64_t now_unix_ms);
  bool AcknowledgeCommand(const std::string& client_id, int64_t revision);
  std::vector<CommandDelivery> GetCommandDeliveries() const;

 private:
  void PruneExpiredEvents();
  void InsertEvent(const std::string& event_type, int64_t timestamp_unix_ms,
                   const std::string& client_id, const std::string& payload_json,
                   std::optional<int> point, std::optional<int> threshold,
                   const std::string& mode);
  DesiredCommand ReadDesiredCommand(const std::string& client_id) const;
  DesiredCommand WriteDesiredCommand(const std::string& client_id, int threshold,
                                     int64_t expires_unix_ms);

  sqlite3* database_ = nullptr;
  int retention_days_;
  mutable std::mutex mutex_;
};

}

#endif
