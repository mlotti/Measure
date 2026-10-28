#include "event_store.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <stdexcept>
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

TEST_CASE("latest desired state and acknowledgments survive restarts") {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("measure-desired-" + std::to_string(nonce) + ".db");
  int64_t revision;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  {
    measure::EventStore store(path.string());
    store.RegisterDevice("sensor-1");
    store.SetDesiredThreshold(6, "tester");
    store.SetDesiredCalibration({"sensor-1"}, now + 10000, "tester");
    const auto command = store.GetDesiredCommand("sensor-1");
    revision = command.revision;
    CHECK(command.expires_unix_ms == now + 10000);
    REQUIRE(store.AcknowledgeCommand("sensor-1", revision));
    CHECK_FALSE(store.AcknowledgeCommand("unknown", revision));
  }
  {
    measure::EventStore store(path.string());
    CHECK(store.GetDesiredCommand("sensor-1").revision == revision);
    CHECK(store.GetCommandDeliveries().front().acknowledged_revision == revision);
    CHECK_FALSE(store.ExpireCalibrations(now + 9000));
    CHECK(store.ExpireCalibrations(now + 10000));
    const auto normal = store.GetDesiredCommand("sensor-1");
    CHECK(normal.revision > revision);
    CHECK(normal.expires_unix_ms == 0);
    CHECK(normal.threshold == 6);
    store.SetDesiredCalibration({"sensor-1"}, now + 20000, "tester");
    store.SetDesiredCalibration({"sensor-1"}, now + 30000, "tester");
    CHECK_FALSE(store.ExpireCalibrations(now + 25000));
    CHECK(store.GetDesiredCommand("sensor-1").expires_unix_ms == now + 30000);
    store.SetDesiredThreshold(4, "tester");
    store.SetDesiredThreshold(2, "tester");
    CHECK_FALSE(store.ExpireCalibrations(now + 40000));
    CHECK(store.GetDesiredCommand("sensor-1").threshold == 2);
    CHECK(store.GetDesiredCommand("new-device").threshold == 2);
    CHECK_FALSE(store.AcknowledgeCommand("sensor-1", revision));
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("measurement retries are deduplicated even after retention and restart") {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("measure-receipts-" + std::to_string(nonce) + ".db");
  {
    measure::EventStore store(path.string(), 1);
    CHECK(store.RecordMeasurement({"sensor-1", 42, 1, "sample-1"}));
    CHECK(store.GetEvents(10).empty());
  }
  {
    measure::EventStore store(path.string(), 1);
    CHECK_FALSE(store.RecordMeasurement({"sensor-1", 42, 1, "sample-1"}));
    CHECK_THROWS_AS(store.RecordMeasurement({"sensor-1", 43, 1, "sample-1"}),
                    std::invalid_argument);
    CHECK(store.RecordMeasurement({"sensor-2", 42, 1, "sample-1"}));
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}
