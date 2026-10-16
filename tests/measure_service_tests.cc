#include "measure_service.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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
                                          int* port) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", TestServerCredentials(), port);
  builder.RegisterService(&service);
  return builder.BuildAndStart();
}

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
  REQUIRE(command_events.size() >= 2);
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
  measure::Thumbs response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  REQUIRE(stub->RecordMeasurement(&context, measurement, &response).ok());
  CHECK(response.response() == 0);

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
  measure::Thumbs response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  const grpc::Status status =
      stub->RecordMeasurement(&context, measurement, &response);
  CHECK(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(service.GetMeasurementsSnapshot().empty());

  server->Shutdown();
}
