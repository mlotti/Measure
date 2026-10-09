#include "event_store.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

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
    Execute(database_, "PRAGMA user_version=1");
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

void EventStore::RecordMeasurement(const StoredMeasurement& measurement) {
  RecordEvent("measurement", measurement.timestamp_unix_ms,
              measurement.client_id,
              "{\"point\":" + std::to_string(measurement.point) + "}",
              measurement.point);
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
    {
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
