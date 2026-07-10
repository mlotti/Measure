#include <iostream>
#include <memory>
#include <string>
#include <random>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "measure.grpc.pb.h"
#else
#include "measure.grpc.pb.h"
#endif

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");
ABSL_FLAG(std::string, client_id, "client1", "Unique client identifier");
ABSL_FLAG(bool, calibrate, false,
          "Operator mode: send a SetCalibrationMode command to the server");
ABSL_FLAG(int, duration, 10, "Calibration duration in seconds (operator mode)");

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using measure::CalibrationRequest;
using measure::CalibrationResponse;
using measure::ClientInfo;
using measure::Command;
using measure::Measure;
using measure::Measurement;
using measure::Mode;
using measure::Thumbs;

class MeasureClient {
 public:
  MeasureClient(std::shared_ptr<Channel> channel, std::string client_id)
      : stub_(Measure::NewStub(channel)),
        client_id_(std::move(client_id)),
        threshold_(8) {}

  // Start background thread that keeps the Subscribe stream open and updates
  // threshold_ whenever the server pushes a Command.
  void StartSubscription() {
    sub_thread_ = std::thread([this]() { RunSubscription(); });
  }

  void StopSubscription() {
    sub_context_.TryCancel();
    if (sub_thread_.joinable()) sub_thread_.join();
  }

  int GetThreshold() const { return threshold_.load(); }

  // Sends a single measurement to the server (async, waits for reply).
  int RecordMeasurement(int value) {
    Measurement measurement;
    measurement.set_point(value);
    Thumbs thumbs;
    ClientContext context;

    std::mutex mu;
    int result = 0;
    std::condition_variable cv;
    bool done = false;

    stub_->async()->RecordMeasurement(
        &context, &measurement, &thumbs,
        [&result, &mu, &cv, &done, &thumbs](Status status) {
          int ret = 0;
          if (!status.ok()) {
            std::cout << "RecordMeasurement RPC failed" << std::endl;
          } else {
            std::cout << "Measurement recieved" << std::endl;
            ret = thumbs.response();
          }
          std::lock_guard<std::mutex> lock(mu);
          result = ret;
          done = true;
          cv.notify_one();
        });
    std::cout << "waiting around" << std::endl;
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&done] { return done; });
    return result;
  }

  // Operator mode: ask the server to push a calibration command to clients.
  bool SetCalibrationMode(const std::string& target_client_id,
                          int duration_seconds) {
    CalibrationRequest request;
    request.set_client_id(target_client_id);
    request.set_duration_seconds(duration_seconds);
    CalibrationResponse response;
    ClientContext context;
    Status status = stub_->SetCalibrationMode(&context, request, &response);
    if (!status.ok()) {
      std::cout << "SetCalibrationMode failed: " << status.error_message()
                << std::endl;
      return false;
    }
    std::cout << "Calibration mode accepted for " << duration_seconds
              << " seconds" << std::endl;
    return response.accepted();
  }

 private:
  // Blocking loop that reads Commands from the server-streaming Subscribe RPC.
  void RunSubscription() {
    ClientInfo info;
    info.set_client_id(client_id_);
    auto reader = stub_->Subscribe(&sub_context_, info);
    Command cmd;
    while (reader->Read(&cmd)) {
      threshold_.store(cmd.threshold());
      std::cout << "Command received: threshold=" << cmd.threshold()
                << " mode="
                << (cmd.mode() == Mode::CALIBRATION ? "CALIBRATION" : "NORMAL")
                << std::endl;
    }
    Status status = reader->Finish();
    if (!status.ok() &&
        status.error_code() != grpc::StatusCode::CANCELLED) {
      std::cout << "Subscribe stream ended: " << status.error_message()
                << std::endl;
    }
  }

  std::unique_ptr<Measure::Stub> stub_;
  std::string client_id_;
  std::atomic<int> threshold_;
  ClientContext sub_context_;
  std::thread sub_thread_;
};

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  std::string target_str = absl::GetFlag(FLAGS_target);
  std::string client_id = absl::GetFlag(FLAGS_client_id);

  MeasureClient measure(
      grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()),
      client_id);

  // Operator mode: send calibration command and exit.
  if (absl::GetFlag(FLAGS_calibrate)) {
    int duration = absl::GetFlag(FLAGS_duration);
    measure.SetCalibrationMode("", duration);  // empty = all clients
    return 0;
  }

  // Sensor mode: subscribe to server commands, then run measurement loop.
  measure.StartSubscription();

  // Wait briefly for the initial threshold command from the server.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 10);

  for (int i = 0; i < 10; i++) {
    int value = dis(gen);
    std::cout << "Generated random number: " << value << std::endl;

    int threshold = measure.GetThreshold();
    if (value > threshold) {
      int reply = measure.RecordMeasurement(value);
      std::cout << "Measured: " << value
                << " (threshold=" << threshold << ")" << std::endl;
      if (reply == 0) std::cout << "Recieved: thumbs up" << std::endl;
      if (reply == 1) std::cout << "Recieved: thumbs down" << std::endl;
    } else {
      std::cout << "Value " << value << " not sent (not above threshold "
                << threshold << ")" << std::endl;
    }
  }

  measure.StopSubscription();
  return 0;
}
