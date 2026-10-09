#include "event_store.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

TEST_CASE("event store supports measurement history, filters, and trends") {
  measure::EventStore store(":memory:");
  store.RecordMeasurement({"sensor-1", 10, 1000});
  store.RecordMeasurement({"sensor-1", 20, 2000});
  store.RecordMeasurement({"sensor-2", 8, 2200});
  store.RecordEvent("threshold", 2300, "", "{\"mode\":\"NORMAL\"}",
                    std::nullopt, 6, "NORMAL");

  const auto history = store.GetEvents(10, 1500, 2300, "sensor-1");
  REQUIRE(history.size() == 1);
  CHECK(history.front().event_type == "measurement");
  CHECK(history.front().point == 20);
  CHECK(store.GetLatestThreshold(8) == 6);

  const auto trends = store.GetTrends(1000);
  REQUIRE(trends.size() == 3);
  CHECK(trends[0].bucket_start_unix_ms == 1000);
  CHECK(trends[0].average == 10.0);
  CHECK(trends[1].bucket_start_unix_ms == 2000);
  CHECK(trends[1].average == 20.0);
  CHECK(trends[2].client_id == "sensor-2");
  const auto latest = store.GetLatestMeasurements();
  REQUIRE(latest.size() == 2);
  CHECK(latest[0].client_id == "sensor-1");
  CHECK(latest[0].point == 20);
}

TEST_CASE("event store persists events and restores the latest threshold") {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("measure-events-" + std::to_string(nonce) + ".db");
  {
    measure::EventStore store(path.string());
    store.RecordMeasurement({"sensor-1", 42, 123456});
    store.RecordEvent("threshold", 123457, "", "{}", std::nullopt, 5,
                      "NORMAL");
  }
  {
    measure::EventStore store(path.string());
    const auto measurements = store.GetMeasurements(10);
    REQUIRE(measurements.size() == 1);
    CHECK(measurements.front().point == 42);
    CHECK(store.GetLatestThreshold(8) == 5);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("event store applies configured retention") {
  measure::EventStore store(":memory:", 1);
  store.RecordMeasurement({"expired", 1, 1});
  store.RecordEvent("threshold", 1, "", "{}", std::nullopt, 6, "NORMAL");
  CHECK(store.GetEvents(10).empty());
  CHECK(store.GetLatestThreshold(8) == 6);
}
