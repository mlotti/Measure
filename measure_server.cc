#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#ifdef BAZEL_BUILD
#include "measure.grpc.pb.h"
#else
#include "measure.grpc.pb.h"
#endif

#include <chrono>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::Status;
using measure::CalibrationRequest;
using measure::CalibrationResponse;
using measure::ClientInfo;
using measure::Command;
using measure::Measure;
using measure::Measurement;
using measure::Mode;
using measure::Thumbs;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

// Default threshold: only measurements above this value are stored.
constexpr int kDefaultThreshold = 8;

void Write(int result) {
  std::ofstream myfile("result.txt", std::ios::app);
  auto t = std::hash<std::thread::id>{}(std::this_thread::get_id());
  if (myfile.is_open()) {
    myfile << result << " " << t << std::endl;
    myfile.close();
  }
}

// Forward declaration so SubscribeReactor can refer to the service.
class MeasureServiceImpl;

// Manages a single client's server-streaming command subscription.
// The server calls PushCommand() to send a Command down the stream.
class SubscribeReactor : public grpc::ServerWriteReactor<Command> {
 public:
  SubscribeReactor(std::string client_id, int initial_threshold,
                   MeasureServiceImpl* service);

  // Thread-safe: enqueue a command and start writing if idle.
  void PushCommand(const Command& cmd) {
    bool should_write = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (done_) return;
      pending_.push(cmd);
      if (!writing_) {
        writing_ = true;
        current_write_ = pending_.front();
        pending_.pop();
        should_write = true;
      }
    }
    if (should_write) StartWrite(&current_write_);
  }

  void OnWriteDone(bool ok) override {
    bool should_write = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      writing_ = false;
      if (!ok || done_) {
        Finish(Status::CANCELLED);
        return;
      }
      if (!pending_.empty()) {
        writing_ = true;
        current_write_ = pending_.front();
        pending_.pop();
        should_write = true;
      }
    }
    if (should_write) StartWrite(&current_write_);
  }

  void OnCancel() override {
    std::lock_guard<std::mutex> lock(mu_);
    done_ = true;
  }

  // Called when the RPC is fully done; unregisters from the service.
  void OnDone() override;

 private:
  std::string client_id_;
  MeasureServiceImpl* service_;
  std::mutex mu_;
  std::queue<Command> pending_;
  Command current_write_;  // must outlive each StartWrite call
  bool writing_ = false;
  bool done_ = false;
};

// Logic and data behind the server's behavior.
class MeasureServiceImpl final : public Measure::CallbackService {
 public:
  void AddSubscriber(const std::string& client_id, SubscribeReactor* reactor) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    subscribers_[client_id] = reactor;
    std::cout << "Client subscribed: " << client_id << std::endl;
  }

  void RemoveSubscriber(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    subscribers_.erase(client_id);
    std::cout << "Client unsubscribed: " << client_id << std::endl;
  }

 private:
  grpc::ServerUnaryReactor* RecordMeasurement(
      grpc::CallbackServerContext* context, const Measurement* request,
      Thumbs* reply) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    Write(request->point());
    reply->set_response(0);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(Status::OK);
    return reactor;
  }

  grpc::ServerWriteReactor<Command>* Subscribe(
      grpc::CallbackServerContext* context,
      const ClientInfo* request) override {
    // Reactor registers itself and sends the current threshold immediately.
    return new SubscribeReactor(request->client_id(), current_threshold_, this);
  }

  grpc::ServerUnaryReactor* SetCalibrationMode(
      grpc::CallbackServerContext* context,
      const CalibrationRequest* request,
      CalibrationResponse* reply) override {
    Command cal_cmd;
    cal_cmd.set_threshold(0);
    cal_cmd.set_mode(Mode::CALIBRATION);

    if (!request->client_id().empty()) {
      SendCommandToClient(request->client_id(), cal_cmd);
    } else {
      BroadcastCommand(cal_cmd);
    }
    std::cout << "Calibration started"
              << (request->client_id().empty()
                      ? " (all clients)"
                      : " (client: " + request->client_id() + ")")
              << " for " << request->duration_seconds() << "s" << std::endl;

    // Revert to normal threshold after the requested duration.
    int duration = request->duration_seconds() > 0 ? request->duration_seconds() : 10;
    int revert_threshold = current_threshold_;
    std::thread([this, duration, revert_threshold]() {
      std::this_thread::sleep_for(std::chrono::seconds(duration));
      Command normal_cmd;
      normal_cmd.set_threshold(revert_threshold);
      normal_cmd.set_mode(Mode::NORMAL);
      BroadcastCommand(normal_cmd);
      std::cout << "Calibration ended, threshold restored to "
                << revert_threshold << std::endl;
    }).detach();

    reply->set_accepted(true);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(Status::OK);
    return reactor;
  }

  void BroadcastCommand(const Command& cmd) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    for (auto& [id, reactor] : subscribers_) {
      reactor->PushCommand(cmd);
    }
  }

  void SendCommandToClient(const std::string& client_id, const Command& cmd) {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    auto it = subscribers_.find(client_id);
    if (it != subscribers_.end()) {
      it->second->PushCommand(cmd);
    }
  }

  std::mutex subscribers_mu_;
  std::map<std::string, SubscribeReactor*> subscribers_;
  int current_threshold_ = kDefaultThreshold;
};

// Out-of-line definitions that need MeasureServiceImpl to be complete.

SubscribeReactor::SubscribeReactor(std::string client_id, int initial_threshold,
                                   MeasureServiceImpl* service)
    : client_id_(std::move(client_id)), service_(service) {
  // Set writing_ = true before registering so a concurrent PushCommand
  // sees a busy reactor and just queues instead of double-writing.
  Command cmd;
  cmd.set_threshold(initial_threshold);
  cmd.set_mode(Mode::NORMAL);
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.push(cmd);
    writing_ = true;
    current_write_ = pending_.front();
    pending_.pop();
  }
  service_->AddSubscriber(client_id_, this);
  StartWrite(&current_write_);
}

void SubscribeReactor::OnDone() {
  service_->RemoveSubscriber(client_id_);
  delete this;
}

void RunServer(uint16_t port) {
  std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
  MeasureServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  server->Wait();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  RunServer(absl::GetFlag(FLAGS_port));
  return 0;
}
