#include "measure_service.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <queue>
#include <thread>
#include <utility>

using grpc::Status;
using measure::CalibrationRequest;
using measure::CalibrationResponse;
using measure::ClientInfo;
using measure::Command;
using measure::Measurement;
using measure::Mode;
using measure::StoredMeasurement;
using measure::Thumbs;

namespace {

int64_t CurrentTimeMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void WriteMeasurement(const StoredMeasurement& measurement) {
  std::ofstream myfile("result.txt", std::ios::app);
  if (myfile.is_open()) {
    myfile << measurement.timestamp_unix_ms << " " << measurement.client_id
           << " " << measurement.point << std::endl;
    myfile.close();
  }
}

}

class SubscribeReactor : public grpc::ServerWriteReactor<Command> {
 public:
  SubscribeReactor(std::string client_id, int initial_threshold,
                   MeasureServiceImpl* service)
      : client_id_(std::move(client_id)), service_(service) {
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

  void OnDone() override {
    service_->RemoveSubscriber(client_id_);
    delete this;
  }

 private:
  std::string client_id_;
  MeasureServiceImpl* service_;
  std::mutex mu_;
  std::queue<Command> pending_;
  Command current_write_;
  bool writing_ = false;
  bool done_ = false;
};

MeasureServiceImpl::MeasureServiceImpl(size_t max_measurements)
    : max_measurements_(max_measurements) {}

void MeasureServiceImpl::AddSubscriber(const std::string& client_id,
                                       SubscribeReactor* reactor) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  subscribers_[client_id] = reactor;
  std::cout << "Client subscribed: " << client_id << std::endl;
}

void MeasureServiceImpl::RemoveSubscriber(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  subscribers_.erase(client_id);
  std::cout << "Client unsubscribed: " << client_id << std::endl;
}

std::vector<StoredMeasurement> MeasureServiceImpl::GetMeasurementsSnapshot()
    const {
  std::lock_guard<std::mutex> lock(measurements_mu_);
  return std::vector<StoredMeasurement>(measurements_.begin(),
                                        measurements_.end());
}

grpc::ServerUnaryReactor* MeasureServiceImpl::RecordMeasurement(
    grpc::CallbackServerContext* context, const Measurement* request,
    Thumbs* reply) {
  std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  StoredMeasurement measurement{
      request->client_id().empty() ? "unknown" : request->client_id(),
      request->point(),
      request->timestamp_unix_ms() > 0 ? request->timestamp_unix_ms()
                                       : CurrentTimeMillis()};
  {
    std::lock_guard<std::mutex> lock(measurements_mu_);
    measurements_.push_back(measurement);
    while (measurements_.size() > max_measurements_) {
      measurements_.pop_front();
    }
  }
  WriteMeasurement(measurement);
  reply->set_response(0);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(Status::OK);
  return reactor;
}

grpc::ServerWriteReactor<Command>* MeasureServiceImpl::Subscribe(
    grpc::CallbackServerContext* context, const ClientInfo* request) {
  return new SubscribeReactor(request->client_id(), current_threshold_, this);
}

grpc::ServerUnaryReactor* MeasureServiceImpl::SetCalibrationMode(
    grpc::CallbackServerContext* context, const CalibrationRequest* request,
    CalibrationResponse* reply) {
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

  int duration = request->duration_seconds() > 0 ? request->duration_seconds() : 10;
  int revert_threshold = current_threshold_;
  std::thread([this, duration, revert_threshold]() {
    std::this_thread::sleep_for(std::chrono::seconds(duration));
    Command normal_cmd;
    normal_cmd.set_threshold(revert_threshold);
    normal_cmd.set_mode(Mode::NORMAL);
    BroadcastCommand(normal_cmd);
    std::cout << "Calibration ended, threshold restored to " << revert_threshold
              << std::endl;
  }).detach();

  reply->set_accepted(true);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(Status::OK);
  return reactor;
}

void MeasureServiceImpl::BroadcastCommand(const Command& cmd) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  for (auto& [id, reactor] : subscribers_) {
    reactor->PushCommand(cmd);
  }
}

void MeasureServiceImpl::SendCommandToClient(const std::string& client_id,
                                             const Command& cmd) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  auto it = subscribers_.find(client_id);
  if (it != subscribers_.end()) {
    it->second->PushCommand(cmd);
  }
}
