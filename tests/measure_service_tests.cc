#include "measure_service.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

TEST_CASE("gRPC subscribers receive threshold and calibration commands") {
  MeasureServiceImpl service(10);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server != nullptr);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  REQUIRE(channel->WaitForConnected(std::chrono::system_clock::now() +
                                    std::chrono::seconds(5)));
  auto stub = measure::Measure::NewStub(channel);

  grpc::ClientContext subscription_context;
  auto reader = stub->Subscribe(&subscription_context,
                                measure::ClientInfo{});
  measure::Command command;
  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 8);
  CHECK(command.mode() == measure::Mode::NORMAL);

  measure::CalibrationRequest request;
  request.set_client_id("subscriber-1");
  request.set_duration_seconds(1);
  measure::CalibrationResponse response;
  grpc::ClientContext calibration_context;
  REQUIRE(stub->SetCalibrationMode(&calibration_context, request, &response)
              .ok());
  CHECK(response.accepted());

  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 0);
  CHECK(command.mode() == measure::Mode::CALIBRATION);

  REQUIRE(reader->Read(&command));
  CHECK(command.threshold() == 8);
  CHECK(command.mode() == measure::Mode::NORMAL);

  subscription_context.TryCancel();
  reader->Finish();
  server->Shutdown();
}

TEST_CASE("gRPC measurement calls acknowledge and store a measurement") {
  MeasureServiceImpl service(10);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server != nullptr);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
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
