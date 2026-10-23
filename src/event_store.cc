#include "event_store.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

#include "measurement_utils.h"

namespace measure {
namespace {

class Statement {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK) {
      sqlite3_finalize(statement_);
      throw std::runtime_error(sqlite3_errmsg(database));
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  sqlite3_stmt* get() const { return statement_; }

  void Check(int result) const {
    if (result != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(database_));
    }
  }

  void Step() const {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw std::runtime_error(sqlite3_errmsg(database_));
    }
  }

 private:
  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};

void Execute(sqlite3* database, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

void BindText(sqlite3_stmt* statement, int index, const std::string& value) {
  if (sqlite3_bind_text(statement, index, value.c_str(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
      SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement)));
  }
}

std::string ColumnText(sqlite3_stmt* statement, int index) {
  const auto* value = sqlite3_column_text(statement, index);
  return value == nullptr ? "" : reinterpret_cast<const char*>(value);
}

int64_t CurrentTimeMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

EventStore::EventStore(const std::string& path, int retention_days)
    : retention_days_(retention_days) {
  if (retention_days < 0 || retention_days > 36500) {
    throw std::invalid_argument("Retention days must be from 0 to 36500.");
  }
  if (path != ":memory:" && !path.empty()) {
    const std::filesystem::path database_path(path);
    if (database_path.has_parent_path()) {
      std::filesystem::create_directories(database_path.parent_path());
    }
  }
  if (sqlite3_open_v2(path.c_str(), &database_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string message =
        database_ == nullptr ? "Unable to open event database."
                             : sqlite3_errmsg(database_);
    if (database_ != nullptr) sqlite3_close(database_);
    database_ = nullptr;
    throw std::runtime_error(message);
  }
  sqlite3_busy_timeout(database_, 5000);
  try {
    Execute(database_, "PRAGMA journal_mode=WAL");
    Execute(database_,
            "CREATE TABLE IF NOT EXISTS events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "event_type TEXT NOT NULL,"
            "timestamp_unix_ms INTEGER NOT NULL,"
            "client_id TEXT NOT NULL DEFAULT '',"
            "point INTEGER,"
            "threshold INTEGER,"
            "mode TEXT NOT NULL DEFAULT '',"
            "payload_json TEXT NOT NULL DEFAULT '{}')");
    Execute(database_,
            "CREATE INDEX IF NOT EXISTS events_type_time "
            "ON events(event_type, timestamp_unix_ms, id)");
    Execute(database_,
            "CREATE INDEX IF NOT EXISTS events_client_time "
            "ON events(client_id, timestamp_unix_ms, id)");
    Execute(database_,
            "CREATE TABLE IF NOT EXISTS settings ("
            "key TEXT PRIMARY KEY, integer_value INTEGER NOT NULL)");
    Execute(database_,
            "CREATE TABLE IF NOT EXISTS measurement_receipts ("
            "client_id TEXT NOT NULL, measurement_id TEXT NOT NULL,"
            "point INTEGER NOT NULL, timestamp_unix_ms INTEGER NOT NULL,"
            "PRIMARY KEY(client_id, measurement_id))");
    Execute(database_,
            "CREATE TABLE IF NOT EXISTS desired_commands ("
            "client_id TEXT PRIMARY KEY, revision INTEGER NOT NULL,"
            "threshold INTEGER NOT NULL, expires_unix_ms INTEGER NOT NULL)");
    Execute(database_,
            "CREATE TABLE IF NOT EXISTS command_delivery ("
            "client_id TEXT PRIMARY KEY, acknowledged_revision INTEGER NOT NULL "
            "DEFAULT 0)");
    const int initial_threshold = GetLatestThreshold(8);
    Statement initial(database_,
                      "INSERT OR IGNORE INTO desired_commands VALUES('',1,?1,0)");
    initial.Check(sqlite3_bind_int(initial.get(), 1, initial_threshold));
    initial.Step();
    Execute(database_,
            "INSERT OR IGNORE INTO settings VALUES('command_revision',1)");
    Execute(database_, "PRAGMA user_version=2");
    PruneExpiredEvents();
  } catch (...) {
    sqlite3_close(database_);
    database_ = nullptr;
    throw;
  }
}

EventStore::~EventStore() {
  if (database_ != nullptr) sqlite3_close(database_);
}

bool EventStore::RecordMeasurement(const StoredMeasurement& measurement) {
  std::lock_guard<std::mutex> lock(mutex_);
  Execute(database_, "BEGIN IMMEDIATE");
  try {
    if (!measurement.measurement_id.empty()) {
      Statement existing(database_,
                         "SELECT point,timestamp_unix_ms FROM measurement_receipts "
                         "WHERE client_id=?1 AND measurement_id=?2");
      BindText(existing.get(), 1, measurement.client_id);
      BindText(existing.get(), 2, measurement.measurement_id);
      const int result = sqlite3_step(existing.get());
      if (result == SQLITE_ROW) {
        if (sqlite3_column_int(existing.get(), 0) != measurement.point ||
            sqlite3_column_int64(existing.get(), 1) !=
                measurement.timestamp_unix_ms) {
          throw std::invalid_argument("measurement_id reused with different data.");
        }
        Execute(database_, "COMMIT");
        return false;
      }
      if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database_));
      Statement receipt(database_,
                        "INSERT INTO measurement_receipts VALUES(?1,?2,?3,?4)");
      BindText(receipt.get(), 1, measurement.client_id);
      BindText(receipt.get(), 2, measurement.measurement_id);
      receipt.Check(sqlite3_bind_int(receipt.get(), 3, measurement.point));
      receipt.Check(sqlite3_bind_int64(receipt.get(), 4,
                                      measurement.timestamp_unix_ms));
      receipt.Step();
    }
    InsertEvent("measurement", measurement.timestamp_unix_ms,
                measurement.client_id,
                "{\"point\":" + std::to_string(measurement.point) +
                    ",\"measurement_id\":\"" +
                    JsonEscape(measurement.measurement_id) + "\"}",
                measurement.point, std::nullopt, "");
    PruneExpiredEvents();
    Execute(database_, "COMMIT");
    return true;
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

void EventStore::RecordEvent(const std::string& event_type,
                             int64_t timestamp_unix_ms,
                             const std::string& client_id,
                             const std::string& payload_json,
                             std::optional<int> point,
                             std::optional<int> threshold,
                             const std::string& mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  Execute(database_, "BEGIN IMMEDIATE");
  try {
    InsertEvent(event_type, timestamp_unix_ms, client_id, payload_json,
                point, threshold, mode);
    if (event_type == "threshold" && threshold.has_value()) {
      Statement statement(
          database_,
          "INSERT OR REPLACE INTO settings(key, integer_value) "
          "VALUES('threshold', ?1)");
      statement.Check(sqlite3_bind_int(statement.get(), 1, *threshold));
      statement.Step();
    }
    PruneExpiredEvents();
    Execute(database_, "COMMIT");
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

void EventStore::InsertEvent(const std::string& event_type,
                             int64_t timestamp_unix_ms,
                             const std::string& client_id,
                             const std::string& payload_json,
                             std::optional<int> point,
                             std::optional<int> threshold,
                             const std::string& mode) {
  Statement statement(
      database_,
      "INSERT INTO events(event_type, timestamp_unix_ms, client_id, point, "
      "threshold, mode, payload_json) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
  BindText(statement.get(), 1, event_type);
  statement.Check(sqlite3_bind_int64(statement.get(), 2, timestamp_unix_ms));
  BindText(statement.get(), 3, client_id);
  if (point.has_value()) {
    statement.Check(sqlite3_bind_int(statement.get(), 4, *point));
  } else {
    statement.Check(sqlite3_bind_null(statement.get(), 4));
  }
  if (threshold.has_value()) {
    statement.Check(sqlite3_bind_int(statement.get(), 5, *threshold));
  } else {
    statement.Check(sqlite3_bind_null(statement.get(), 5));
  }
  BindText(statement.get(), 6, mode);
  BindText(statement.get(), 7, payload_json);
  statement.Step();
}

DesiredCommand EventStore::ReadDesiredCommand(const std::string& client_id) const {
  Statement statement(database_,
                      "SELECT revision,threshold,expires_unix_ms FROM "
                      "desired_commands WHERE client_id IN ('',?1) "
                      "ORDER BY revision DESC LIMIT 1");
  BindText(statement.get(), 1, client_id);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error("Unable to load desired command.");
  }
  return {sqlite3_column_int64(statement.get(), 0),
          sqlite3_column_int(statement.get(), 1),
          sqlite3_column_int64(statement.get(), 2)};
}

DesiredCommand EventStore::GetDesiredCommand(const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadDesiredCommand(client_id);
}

DesiredCommand EventStore::WriteDesiredCommand(const std::string& client_id,
                                               int threshold,
                                               int64_t expires_unix_ms) {
  Execute(database_,
          "UPDATE settings SET integer_value=integer_value+1 "
          "WHERE key='command_revision'");
  Statement revision(database_,
                     "SELECT integer_value FROM settings WHERE key='command_revision'");
  if (sqlite3_step(revision.get()) != SQLITE_ROW) {
    throw std::runtime_error("Unable to allocate command revision.");
  }
  DesiredCommand command{sqlite3_column_int64(revision.get(), 0), threshold,
                         expires_unix_ms};
  Statement statement(database_,
                      "INSERT OR REPLACE INTO desired_commands VALUES(?1,?2,?3,?4)");
  BindText(statement.get(), 1, client_id);
  statement.Check(sqlite3_bind_int64(statement.get(), 2, command.revision));
  statement.Check(sqlite3_bind_int(statement.get(), 3, threshold));
  statement.Check(sqlite3_bind_int64(statement.get(), 4, expires_unix_ms));
  statement.Step();
  return command;
}

void EventStore::RegisterDevice(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      "INSERT OR IGNORE INTO command_delivery(client_id) VALUES(?1)");
  BindText(statement.get(), 1, client_id);
  statement.Step();
}

void EventStore::SetDesiredThreshold(int threshold, const std::string& actor) {
  std::lock_guard<std::mutex> lock(mutex_);
  Execute(database_, "BEGIN IMMEDIATE");
  try {
    Execute(database_, "DELETE FROM desired_commands");
    const auto command = WriteDesiredCommand("", threshold, 0);
    Statement setting(database_,
                      "INSERT OR REPLACE INTO settings VALUES('threshold',?1)");
    setting.Check(sqlite3_bind_int(setting.get(), 1, threshold));
    setting.Step();
    InsertEvent("threshold", CurrentTimeMillis(), "",
                "{\"action\":\"threshold_updated\",\"revision\":" +
                    std::to_string(command.revision) + ",\"actor\":\"" +
                    JsonEscape(actor) + "\"}",
                std::nullopt, threshold, "NORMAL");
    PruneExpiredEvents();
    Execute(database_, "COMMIT");
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

void EventStore::SetDesiredCalibration(const std::vector<std::string>& clients,
                                       int64_t expires_unix_ms,
                                       const std::string& actor) {
  std::lock_guard<std::mutex> lock(mutex_);
  Execute(database_, "BEGIN IMMEDIATE");
  try {
    for (const auto& client : clients) {
      const auto command = WriteDesiredCommand(client, 0, expires_unix_ms);
      InsertEvent("command", CurrentTimeMillis(), client,
                  "{\"action\":\"calibration_started\",\"revision\":" +
                      std::to_string(command.revision) +
                      ",\"expires_unix_ms\":" + std::to_string(expires_unix_ms) +
                      ",\"actor\":\"" + JsonEscape(actor) + "\"}",
                  std::nullopt, 0, "CALIBRATION");
    }
    PruneExpiredEvents();
    Execute(database_, "COMMIT");
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

bool EventStore::ExpireCalibrations(int64_t now_unix_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  Execute(database_, "BEGIN IMMEDIATE");
  try {
    std::vector<std::string> expired;
    {
      Statement statement(database_,
                          "SELECT client_id FROM desired_commands "
                          "WHERE expires_unix_ms>0 AND expires_unix_ms<=?1");
      statement.Check(sqlite3_bind_int64(statement.get(), 1, now_unix_ms));
      int result;
      while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        expired.push_back(ColumnText(statement.get(), 0));
      }
      if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database_));
    }
    const int threshold = ReadDesiredCommand("").threshold;
    for (const auto& client : expired) {
      const auto command = WriteDesiredCommand(client, threshold, 0);
      InsertEvent("command", now_unix_ms, client,
                  "{\"action\":\"calibration_ended\",\"revision\":" +
                      std::to_string(command.revision) + "}",
                  std::nullopt, threshold, "NORMAL");
    }
    if (!expired.empty()) PruneExpiredEvents();
    Execute(database_, "COMMIT");
    return !expired.empty();
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

bool EventStore::AcknowledgeCommand(const std::string& client_id,
                                    int64_t revision) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement device(database_,
                   "SELECT 1 FROM command_delivery WHERE client_id=?1");
  BindText(device.get(), 1, client_id);
  const int device_result = sqlite3_step(device.get());
  if (device_result == SQLITE_DONE) return false;
  if (device_result != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(database_));
  }
  const auto desired = ReadDesiredCommand(client_id);
  if (desired.revision != revision ||
      (desired.expires_unix_ms > 0 &&
       desired.expires_unix_ms <= CurrentTimeMillis())) {
    return false;
  }
  Execute(database_, "BEGIN IMMEDIATE");
  try {
    Statement statement(database_,
                        "UPDATE command_delivery SET acknowledged_revision=?2 "
                        "WHERE client_id=?1 AND acknowledged_revision<>?2");
    BindText(statement.get(), 1, client_id);
    statement.Check(sqlite3_bind_int64(statement.get(), 2, revision));
    statement.Step();
    if (sqlite3_changes(database_) != 0) {
      InsertEvent("command_acknowledged", CurrentTimeMillis(), client_id,
                  "{\"action\":\"command_applied\",\"revision\":" +
                      std::to_string(revision) + "}",
                  std::nullopt, desired.threshold,
                  desired.expires_unix_ms > 0 ? "CALIBRATION" : "NORMAL");
      PruneExpiredEvents();
    }
    Execute(database_, "COMMIT");
    return true;
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::vector<CommandDelivery> EventStore::GetCommandDeliveries() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(database_,
                      "SELECT client_id,acknowledged_revision FROM command_delivery "
                      "ORDER BY client_id");
  std::vector<CommandDelivery> deliveries;
  int result;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto client = ColumnText(statement.get(), 0);
    deliveries.push_back({client, ReadDesiredCommand(client).revision,
                          sqlite3_column_int64(statement.get(), 1)});
  }
  if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database_));
  return deliveries;
}

std::vector<StoredMeasurement> EventStore::GetMeasurements(size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      database_,
      "SELECT client_id, point, timestamp_unix_ms FROM "
      "(SELECT client_id, point, timestamp_unix_ms, id FROM events "
      "WHERE event_type = 'measurement' AND point IS NOT NULL "
      "ORDER BY timestamp_unix_ms DESC, id DESC LIMIT ?1) "
      "ORDER BY timestamp_unix_ms ASC, id ASC");
  statement.Check(sqlite3_bind_int64(
      statement.get(), 1, static_cast<sqlite3_int64>(limit)));
  std::vector<StoredMeasurement> measurements;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    measurements.push_back({ColumnText(statement.get(), 0),
                            sqlite3_column_int(statement.get(), 1),
                            sqlite3_column_int64(statement.get(), 2)});
  }
  if (result != SQLITE_DONE) {
    throw std::runtime_error(sqlite3_errmsg(database_));
  }
  return measurements;
}

std::vector<StoredMeasurement> EventStore::GetLatestMeasurements() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      database_,
      "SELECT event.client_id, event.point, event.timestamp_unix_ms FROM "
      "events AS event WHERE event.event_type = 'measurement' "
      "AND event.point IS NOT NULL AND NOT EXISTS ("
      "SELECT 1 FROM events AS newer WHERE newer.event_type = 'measurement' "
      "AND newer.client_id = event.client_id "
      "AND (newer.timestamp_unix_ms > event.timestamp_unix_ms OR "
      "(newer.timestamp_unix_ms = event.timestamp_unix_ms AND newer.id > "
      "event.id))) ORDER BY event.client_id");
  std::vector<StoredMeasurement> measurements;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    measurements.push_back({ColumnText(statement.get(), 0),
                            sqlite3_column_int(statement.get(), 1),
                            sqlite3_column_int64(statement.get(), 2)});
  }
  if (result != SQLITE_DONE) {
    throw std::runtime_error(sqlite3_errmsg(database_));
  }
  return measurements;
}

std::vector<StoredEvent> EventStore::GetEvents(size_t limit,
                                               int64_t since_unix_ms,
                                               int64_t until_unix_ms,
                                               const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      database_,
      "SELECT id, event_type, timestamp_unix_ms, client_id, point, threshold, "
      "mode, payload_json FROM (SELECT id, event_type, timestamp_unix_ms, "
      "client_id, point, threshold, mode, payload_json FROM events "
      "WHERE (?1 = 0 OR timestamp_unix_ms >= ?1) "
      "AND (?2 = 0 OR timestamp_unix_ms <= ?2) "
      "AND (?3 = '' OR client_id = ?3) "
      "ORDER BY timestamp_unix_ms DESC, id DESC LIMIT ?4) "
      "ORDER BY timestamp_unix_ms ASC, id ASC");
  statement.Check(sqlite3_bind_int64(statement.get(), 1, since_unix_ms));
  statement.Check(sqlite3_bind_int64(statement.get(), 2, until_unix_ms));
  BindText(statement.get(), 3, client_id);
  statement.Check(sqlite3_bind_int64(
      statement.get(), 4, static_cast<sqlite3_int64>(limit)));
  std::vector<StoredEvent> events;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    StoredEvent event;
    event.id = sqlite3_column_int64(statement.get(), 0);
    event.event_type = ColumnText(statement.get(), 1);
    event.timestamp_unix_ms = sqlite3_column_int64(statement.get(), 2);
    event.client_id = ColumnText(statement.get(), 3);
    if (sqlite3_column_type(statement.get(), 4) != SQLITE_NULL) {
      event.point = sqlite3_column_int(statement.get(), 4);
    }
    if (sqlite3_column_type(statement.get(), 5) != SQLITE_NULL) {
      event.threshold = sqlite3_column_int(statement.get(), 5);
    }
    event.mode = ColumnText(statement.get(), 6);
    event.payload_json = ColumnText(statement.get(), 7);
    events.push_back(std::move(event));
  }
  if (result != SQLITE_DONE) {
    throw std::runtime_error(sqlite3_errmsg(database_));
  }
  return events;
}

std::vector<MeasurementTrend> EventStore::GetTrends(
    int64_t bucket_ms, int64_t since_unix_ms, int64_t until_unix_ms,
    const std::string& client_id) const {
  if (bucket_ms < 1) throw std::invalid_argument("Trend bucket must be positive.");
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      database_,
      "SELECT client_id, CAST(timestamp_unix_ms / ?1 AS INTEGER) * ?1, "
      "COUNT(*), AVG(point), MIN(point), MAX(point) FROM events "
      "WHERE event_type = 'measurement' AND point IS NOT NULL "
      "AND (?2 = 0 OR timestamp_unix_ms >= ?2) "
      "AND (?3 = 0 OR timestamp_unix_ms <= ?3) "
      "AND (?4 = '' OR client_id = ?4) "
      "GROUP BY client_id, CAST(timestamp_unix_ms / ?1 AS INTEGER) "
      "ORDER BY 2, client_id");
  statement.Check(sqlite3_bind_int64(statement.get(), 1, bucket_ms));
  statement.Check(sqlite3_bind_int64(statement.get(), 2, since_unix_ms));
  statement.Check(sqlite3_bind_int64(statement.get(), 3, until_unix_ms));
  BindText(statement.get(), 4, client_id);
  std::vector<MeasurementTrend> trends;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    trends.push_back({ColumnText(statement.get(), 0),
                      sqlite3_column_int64(statement.get(), 1),
                      sqlite3_column_int(statement.get(), 2),
                      sqlite3_column_double(statement.get(), 3),
                      sqlite3_column_int(statement.get(), 4),
                      sqlite3_column_int(statement.get(), 5)});
  }
  if (result != SQLITE_DONE) {
    throw std::runtime_error(sqlite3_errmsg(database_));
  }
  return trends;
}

int EventStore::GetLatestThreshold(int fallback) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      database_,
      "SELECT integer_value FROM settings WHERE key = 'threshold'");
  const int result = sqlite3_step(statement.get());
  if (result == SQLITE_ROW) return sqlite3_column_int(statement.get(), 0);
  if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database_));
  Statement latest(
      database_,
      "SELECT threshold FROM events WHERE event_type = 'threshold' "
      "AND threshold IS NOT NULL ORDER BY timestamp_unix_ms DESC, id DESC "
      "LIMIT 1");
  const int latest_result = sqlite3_step(latest.get());
  if (latest_result == SQLITE_ROW) return sqlite3_column_int(latest.get(), 0);
  if (latest_result != SQLITE_DONE) {
    throw std::runtime_error(sqlite3_errmsg(database_));
  }
  return fallback;
}

void EventStore::PruneExpiredEvents() {
  if (retention_days_ == 0) return;
  const int64_t retention_ms =
      static_cast<int64_t>(retention_days_) * 24 * 60 * 60 * 1000;
  Statement statement(database_, "DELETE FROM events WHERE timestamp_unix_ms < ?1");
  statement.Check(
      sqlite3_bind_int64(statement.get(), 1, CurrentTimeMillis() - retention_ms));
  statement.Step();
}

}
