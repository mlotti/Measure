#include "measure_service.h"
#include "measure_client.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string ReadTestFile(const std::string& name) {
  std::ifstream input(std::string(MEASURE_TEST_DATA_DIR) + "/" + name,
                      std::ios::binary);
  REQUIRE(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::shared_ptr<grpc::ServerCredentials> TestServerCredentials() {
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = ReadTestFile("ca.crt");
  options.pem_key_cert_pairs.push_back(
      {ReadTestFile("server.key"), ReadTestFile("server.crt")});
  options.client_certificate_request =
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  return grpc::SslServerCredentials(options);
}

std::shared_ptr<grpc::Channel> TestChannel(int port, bool operator_identity) {
  grpc::SslCredentialsOptions options;
  options.pem_root_certs = ReadTestFile("ca.crt");
  options.pem_cert_chain =
      ReadTestFile(operator_identity ? "operator.crt" : "device.crt");
  options.pem_private_key =
      ReadTestFile(operator_identity ? "operator.key" : "device.key");
  return grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                             grpc::SslCredentials(options));
}

std::unique_ptr<grpc::Server> StartServer(MeasureServiceImpl& service,
                                          int* port, int requested_port = 0) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:" + std::to_string(requested_port),
                           TestServerCredentials(), port);
  builder.RegisterService(&service);
  return builder.BuildAndStart();
}

bool Eventually(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(8)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

struct TemporaryDatabase {
  std::string path = (std::filesystem::temp_directory_path() /
                     ("measure-recovery-" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()) +
                      ".db")).string();
  ~TemporaryDatabase() {
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
  }
};

struct DatabaseWriteLock {
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{nullptr, sqlite3_close};

  explicit DatabaseWriteLock(const std::string& path) {
    sqlite3* connection = nullptr;
    const int result = sqlite3_open(path.c_str(), &connection);
    database.reset(connection);
    REQUIRE(result == SQLITE_OK);
    REQUIRE(sqlite3_exec(database.get(), "BEGIN IMMEDIATE", nullptr, nullptr,
                         nullptr) == SQLITE_OK);
  }

  void Release() {
    REQUIRE(sqlite3_exec(database.get(), "ROLLBACK", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
  }
};

}

TEST_CASE("gRPC subscribers receive threshold and calibration commands") {
  MeasureServiceImpl service(10);
  int port = 0;
  std::unique_ptr<grpc::Server> server = StartServer(service, &port);
  REQUIRE(server != nullptr);

  auto channel = TestChannel(port, false);
  REQUIRE(channel->WaitForConnected(std::chrono::system_clock::now() +
                                    std::chrono::seconds(5)));
  auto stub = measure::Measure::NewStub(channel);

  grpc::ClientContext subscription_context;
  subscription_context.set_deadline(std::chrono::system_clock::now() +
                                    std::chrono::seconds(5));
  measure::ClientInfo client_info;
  client_info.set_client_id("sensor-1");
  auto reader = stub->Subscribe(&subscription_context, client_info);
  measure::Command command;
  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 8);
  CHECK(command.mode() == measure::Mode::NORMAL);
  const auto connected_clients = service.GetConnectedClients();
  REQUIRE(connected_clients.size() == 1);
  CHECK(connected_clients.front() == "sensor-1");

  service.SetThreshold(6);
  CHECK(service.GetThreshold() == 6);
  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 6);
  CHECK(command.mode() == measure::Mode::NORMAL);

  measure::CalibrationRequest request;
  request.set_client_id("sensor-1");
  request.set_duration_seconds(1);
  measure::CalibrationResponse response;
  grpc::ClientContext unauthorized_context;
  CHECK(stub->SetCalibrationMode(&unauthorized_context, request, &response)
            .error_code() == grpc::StatusCode::PERMISSION_DENIED);

  grpc::ClientContext calibration_context;
  auto operator_stub =
      measure::Measure::NewStub(TestChannel(port, true));
  REQUIRE(operator_stub->SetCalibrationMode(&calibration_context, request, &response)
              .ok());
  CHECK(response.accepted());

  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 0);
  CHECK(command.mode() == measure::Mode::CALIBRATION);
  const auto command_events = service.GetEventsSnapshot(10, 0, 0, "sensor-1");
  REQUIRE(command_events.size() == 1);
  CHECK(command_events.front().payload_json.find("operator:tester") !=
        std::string::npos);

  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 6);
  CHECK(command.mode() == measure::Mode::NORMAL);

  subscription_context.TryCancel();
  reader->Finish();
  server->Shutdown();
}

TEST_CASE("gRPC measurement calls acknowledge and store a measurement") {
  MeasureServiceImpl service(10);
  int port = 0;
  std::unique_ptr<grpc::Server> server = StartServer(service, &port);
  REQUIRE(server != nullptr);

  auto channel = TestChannel(port, false);
  REQUIRE(channel->WaitForConnected(std::chrono::system_clock::now() +
                                    std::chrono::seconds(5)));
  auto stub = measure::Measure::NewStub(channel);

  measure::Measurement measurement;
  measurement.set_client_id("sensor-1");
  measurement.set_point(42);
  measurement.set_timestamp_unix_ms(123456);
  measurement.set_measurement_id("sample-1");
  measure::MeasurementReceipt response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  REQUIRE(stub->RecordMeasurement(&context, measurement, &response).ok());
  CHECK_FALSE(response.duplicate());
  CHECK(response.measurement_id() == "sample-1");
  grpc::ClientContext retry_context;
  REQUIRE(stub->RecordMeasurement(&retry_context, measurement, &response).ok());
  CHECK(response.duplicate());

  measurement.set_point(43);
  grpc::ClientContext conflict_context;
  CHECK(stub->RecordMeasurement(&conflict_context, measurement, &response)
            .error_code() == grpc::StatusCode::ALREADY_EXISTS);
  measurement.set_point(42);
  grpc::ClientContext after_error_context;
  REQUIRE(stub->RecordMeasurement(&after_error_context, measurement, &response).ok());
  CHECK(response.duplicate());

  const auto stored = service.GetMeasurementsSnapshot();
  REQUIRE(stored.size() == 1);
  CHECK(stored.front().client_id == "sensor-1");
  CHECK(stored.front().point == 42);
  CHECK(stored.front().timestamp_unix_ms == 123456);

  server->Shutdown();
}

TEST_CASE("device certificate identity cannot be spoofed") {
  MeasureServiceImpl service(10);
  int port = 0;
  std::unique_ptr<grpc::Server> server = StartServer(service, &port);
  REQUIRE(server != nullptr);

  auto channel = TestChannel(port, false);
  REQUIRE(channel->WaitForConnected(std::chrono::system_clock::now() +
                                   std::chrono::seconds(5)));
  auto stub = measure::Measure::NewStub(channel);
  measure::Measurement measurement;
  measurement.set_client_id("another-device");
  measurement.set_point(42);
  measure::MeasurementReceipt response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  const grpc::Status status =
      stub->RecordMeasurement(&context, measurement, &response);
  CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(service.GetMeasurementsSnapshot().empty());

  server->Shutdown();
}

TEST_CASE("reliable commands are retried until applied and stale acknowledgments fail") {
  MeasureServiceImpl service(10);
  int port = 0;
  auto server = StartServer(service, &port);
  REQUIRE(server != nullptr);
  auto stub = measure::Measure::NewStub(TestChannel(port, false));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
  measure::ClientInfo info;
  info.set_client_id("sensor-1");
  info.set_protocol_version(1);
  auto reader = stub->Subscribe(&context, info);
  measure::Command command;
  REQUIRE(reader->Read(&command));
  const auto initial_revision = command.revision();
  CHECK(command.protocol_version() == 1);
  REQUIRE(reader->Read(&command));
  CHECK(command.revision() == initial_revision);

  measure::CommandAcknowledgment ack;
  ack.set_client_id("sensor-1");
  ack.set_revision(initial_revision);
  measure::CommandReceipt receipt;
  grpc::ClientContext ack_context;
  REQUIRE(stub->AcknowledgeCommand(&ack_context, ack, &receipt).ok());
  REQUIRE(service.GetCommandDeliveries().size() == 1);
  CHECK(service.GetCommandDeliveries().front().acknowledged_revision ==
        initial_revision);
  grpc::ClientContext duplicate_context;
  REQUIRE(stub->AcknowledgeCommand(&duplicate_context, ack, &receipt).ok());
  const auto events = service.GetEventsSnapshot(20);
  CHECK(std::count_if(events.begin(), events.end(), [](const auto& event) {
    return event.event_type == "command_acknowledged";
  }) == 1);

  service.SetThreshold(4);
  grpc::ClientContext stale_context;
  CHECK(stub->AcknowledgeCommand(&stale_context, ack, &receipt).error_code() ==
        grpc::StatusCode::FAILED_PRECONDITION);
  ack.set_client_id("another-device");
  grpc::ClientContext spoof_context;
  CHECK(stub->AcknowledgeCommand(&spoof_context, ack, &receipt).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);

  context.TryCancel();
  reader->Finish();
  grpc::ClientContext unsupported_context;
  info.set_protocol_version(2);
  auto unsupported = stub->Subscribe(&unsupported_context, info);
  CHECK_FALSE(unsupported->Read(&command));
  CHECK(unsupported->Finish().error_code() ==
        grpc::StatusCode::FAILED_PRECONDITION);
  server->Shutdown();
}

TEST_CASE("an old subscription cannot unregister its replacement") {
  MeasureServiceImpl service(10);
  int port = 0;
  auto server = StartServer(service, &port);
  REQUIRE(server != nullptr);
  auto stub = measure::Measure::NewStub(TestChannel(port, false));
  measure::ClientInfo info;
  info.set_client_id("sensor-1");
  grpc::ClientContext first_context;
  first_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(5));
  auto first = stub->Subscribe(&first_context, info);
  measure::Command command;
  REQUIRE(first->Read(&command));
  grpc::ClientContext second_context;
  second_context.set_deadline(std::chrono::system_clock::now() +
                               std::chrono::seconds(5));
  auto second = stub->Subscribe(&second_context, info);
  REQUIRE(second->Read(&command));
  first_context.TryCancel();
  first->Finish();
  REQUIRE(Eventually([&] { return service.GetConnectedClients().size() == 1; }));
  service.SetThreshold(3);
  REQUIRE(second->Read(&command));
  CHECK(command.threshold() == 3);
  second_context.TryCancel();
  second->Finish();
  server->Shutdown();
}

TEST_CASE("client reconnects after restart and uploads buffered measurements") {
  TemporaryDatabase database;
  auto service = std::make_unique<MeasureServiceImpl>(10, database.path);
  int port = 0;
  auto server = StartServer(*service, &port);
  REQUIRE(server != nullptr);
  MeasureClient client(TestChannel(port, false), "sensor-1");
  client.Start();
  REQUIRE(client.WaitForCommand(std::chrono::seconds(5)));
  REQUIRE(Eventually([&] {
    const auto deliveries = service->GetCommandDeliveries();
    return deliveries.size() == 1 &&
           deliveries.front().desired_revision ==
               deliveries.front().acknowledged_revision;
  }));
  service->StartCalibration("sensor-1", 1);
  REQUIRE(Eventually([&] { return client.GetThreshold() == 0; }));
  server->Shutdown(std::chrono::system_clock::now());
  server.reset();
  service.reset();
  REQUIRE(client.RecordMeasurement(42));
  REQUIRE(Eventually([&] { return client.GetThreshold() == 8; }));
  CHECK(client.PendingMeasurements() == 1);

  service = std::make_unique<MeasureServiceImpl>(10, database.path);
  service->SetThreshold(6);
  service->SetThreshold(3);
  server = StartServer(*service, &port, port);
  REQUIRE(server != nullptr);
  REQUIRE(Eventually([&] { return client.GetThreshold() == 3; }));
  REQUIRE(client.WaitForMeasurements(std::chrono::seconds(8)));
  const auto measurements = service->GetMeasurementsSnapshot();
  REQUIRE(measurements.size() == 1);
  CHECK(measurements.front().point == 42);
  REQUIRE(Eventually([&] {
    const auto deliveries = service->GetCommandDeliveries();
    return deliveries.front().desired_revision ==
           deliveries.front().acknowledged_revision;
  }));
  client.Stop();
  server->Shutdown();
}

TEST_CASE("offline measurement buffering is bounded and never silently drops data") {
  MeasureClient client(grpc::CreateChannel("127.0.0.1:1",
                                            grpc::InsecureChannelCredentials()),
                        "sensor-1");
  for (int i = 0; i < 1000; ++i) REQUIRE(client.RecordMeasurement(i));
  CHECK_FALSE(client.RecordMeasurement(1001));
  CHECK(client.PendingMeasurements() == 1000);
  CHECK_FALSE(client.WaitForMeasurements(std::chrono::milliseconds(1)));
}

TEST_CASE("reconnecting during calibration preserves its original deadline") {
  MeasureServiceImpl service(10);
  int port = 0;
  auto server = StartServer(service, &port);
  REQUIRE(server != nullptr);
  auto stub = measure::Measure::NewStub(TestChannel(port, false));
  measure::ClientInfo info;
  info.set_client_id("sensor-1");
  grpc::ClientContext first_context;
  first_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(5));
  auto first = stub->Subscribe(&first_context, info);
  measure::Command command;
  REQUIRE(first->Read(&command));
  service.StartCalibration("sensor-1", 1);
  REQUIRE(first->Read(&command));
  REQUIRE(command.mode() == measure::CALIBRATION);
  const auto revision = command.revision();
  const auto expiry = command.expires_unix_ms();
  first_context.TryCancel();
  first->Finish();

  grpc::ClientContext second_context;
  second_context.set_deadline(std::chrono::system_clock::now() +
                               std::chrono::seconds(5));
  auto second = stub->Subscribe(&second_context, info);
  REQUIRE(second->Read(&command));
  CHECK(command.mode() == measure::CALIBRATION);
  CHECK(command.revision() == revision);
  CHECK(command.expires_unix_ms() == expiry);
  REQUIRE(second->Read(&command));
  CHECK(command.mode() == measure::NORMAL);
  CHECK(command.revision() > revision);
  CHECK(command.threshold() == 8);
  second_context.TryCancel();
  second->Finish();
  server->Shutdown();
}

TEST_CASE("measurement admission rejects overflow and clients retry without data loss") {
  const size_t limit = GENERATE(1, 4);
  CAPTURE(limit);
  TemporaryDatabase database;
  auto service_instance = limit == 4
      ? std::make_unique<MeasureServiceImpl>(100, database.path)
      : std::make_unique<MeasureServiceImpl>(100, database.path, 0, limit);
  auto& service = *service_instance;
  int port = 0;
  auto server = StartServer(service, &port);
  REQUIRE(server != nullptr);
  auto channel = TestChannel(port, false);
  MeasureClient client(channel, "sensor-1");
  client.Start();
  REQUIRE(client.WaitForCommand(std::chrono::seconds(5)));
  REQUIRE(Eventually([&] {
    const auto deliveries = service.GetCommandDeliveries();
    return deliveries.size() == 1 &&
           deliveries.front().desired_revision ==
               deliveries.front().acknowledged_revision;
  }));

  // Hold SQLite writes so admitted handlers cannot finish during the burst.
  DatabaseWriteLock write_lock(database.path);
  constexpr size_t requests = 16;
  std::atomic<size_t> rejected{0};
  std::vector<std::future<grpc::Status>> calls;
  for (size_t i = 0; i < requests; ++i) {
    calls.push_back(std::async(std::launch::async, [&, i] {
      auto stub = measure::Measure::NewStub(channel);
      measure::Measurement measurement;
      measurement.set_client_id("sensor-1");
      measurement.set_point(static_cast<int>(i));
      measurement.set_timestamp_unix_ms(123456);
      measurement.set_measurement_id("burst-" + std::to_string(i));
      measure::MeasurementReceipt receipt;
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() +
                            std::chrono::seconds(10));
      auto status = stub->RecordMeasurement(&context, measurement, &receipt);
      if (status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED) ++rejected;
      return status;
    }));
  }
  const bool overflow_rejected = Eventually(
      [&] { return rejected.load() == requests - limit; }, std::chrono::seconds(2));
  CHECK(overflow_rejected);
  const bool queued = client.RecordMeasurement(42);
  CHECK(queued);
  CHECK_FALSE(client.WaitForMeasurements(std::chrono::milliseconds(250)));
  CHECK(client.PendingMeasurements() == 1);
  write_lock.Release();

  size_t accepted = 0;
  for (auto& call : calls) {
    const auto status = call.get();
    if (status.ok()) {
      ++accepted;
    } else {
      CHECK(status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
    }
  }
  CHECK(accepted == limit);
  REQUIRE(client.WaitForMeasurements(std::chrono::seconds(8)));
  CHECK(client.PendingMeasurements() == 0);
  const auto stored = service.GetMeasurementsSnapshot();
  CHECK(stored.size() == accepted + 1);
  CHECK(std::count_if(stored.begin(), stored.end(), [](const auto& measurement) {
    return measurement.point == 42;
  }) == 1);
  client.Stop();
  server->Shutdown();
}

TEST_CASE("measurement concurrency limit must be positive") {
  CHECK_THROWS_AS(MeasureServiceImpl(10, ":memory:", 0, 0),
                  std::invalid_argument);
}

TEST_CASE("measurement admission capacity is released after storage errors") {
  TemporaryDatabase database;
  MeasureServiceImpl service(10, database.path, 0, 1);
  {
    DatabaseWriteLock write_lock(database.path);
    REQUIRE(sqlite3_exec(
                write_lock.database.get(),
                "CREATE TRIGGER reject_sample BEFORE INSERT ON events "
                "WHEN NEW.point=42 BEGIN SELECT RAISE(ABORT,'test write failure'); END",
                nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(write_lock.database.get(), "COMMIT", nullptr, nullptr,
                         nullptr) == SQLITE_OK);
  }
  int port = 0;
  auto server = StartServer(service, &port);
  REQUIRE(server != nullptr);
  auto stub = measure::Measure::NewStub(TestChannel(port, false));
  measure::Measurement measurement;
  measurement.set_client_id("sensor-1");
  measurement.set_measurement_id("failed-then-retried");
  measurement.set_timestamp_unix_ms(123456);
  measurement.set_point(42);
  measure::MeasurementReceipt receipt;
  grpc::ClientContext failed_context;
  failed_context.set_deadline(std::chrono::system_clock::now() +
                               std::chrono::seconds(5));
  CHECK(stub->RecordMeasurement(&failed_context, measurement, &receipt)
            .error_code() == grpc::StatusCode::INTERNAL);
  CHECK(service.GetMeasurementsSnapshot().empty());

  measurement.set_point(43);
  grpc::ClientContext retry_context;
  retry_context.set_deadline(std::chrono::system_clock::now() +
                              std::chrono::seconds(5));
  REQUIRE(stub->RecordMeasurement(&retry_context, measurement, &receipt).ok());
  CHECK_FALSE(receipt.duplicate());
  CHECK(receipt.measurement_id() == measurement.measurement_id());
  CHECK(service.GetMeasurementsSnapshot().size() == 1);
  server->Shutdown();
}
