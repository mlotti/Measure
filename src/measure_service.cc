#include "measure_service.h"

#include <chrono>
#include <exception>
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
    bool should_finish = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      done_ = true;
      should_finish = !writing_;
    }
    if (should_finish) Finish(Status::CANCELLED);
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

MeasureServiceImpl::MeasureServiceImpl(size_t max_measurements,
                                       const std::string& database_path,
                                       int retention_days)
    : event_store_(database_path, retention_days),
      max_measurements_(max_measurements),
      current_threshold_(event_store_.GetLatestThreshold(8)) {}

MeasureServiceImpl::~MeasureServiceImpl() {
  {
    std::lock_guard<std::mutex> lock(calibration_mu_);
    shutting_down_ = true;
  }
  calibration_cv_.notify_all();
  for (auto& thread : calibration_threads_) {
    if (thread.joinable()) thread.join();
  }
}

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
  return event_store_.GetMeasurements(max_measurements_);
}

std::vector<StoredMeasurement>
MeasureServiceImpl::GetLatestMeasurementsSnapshot() const {
  return event_store_.GetLatestMeasurements();
}

std::vector<measure::StoredEvent> MeasureServiceImpl::GetEventsSnapshot(
    size_t limit, int64_t since_unix_ms, int64_t until_unix_ms,
    const std::string& client_id) const {
  return event_store_.GetEvents(limit, since_unix_ms, until_unix_ms, client_id);
}

std::vector<measure::MeasurementTrend> MeasureServiceImpl::GetTrends(
    int64_t bucket_ms, int64_t since_unix_ms, int64_t until_unix_ms,
    const std::string& client_id) const {
  return event_store_.GetTrends(bucket_ms, since_unix_ms, until_unix_ms,
                                client_id);
}

std::vector<std::string> MeasureServiceImpl::GetConnectedClients() const {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  std::vector<std::string> clients;
  clients.reserve(subscribers_.size());
  for (const auto& subscriber : subscribers_) {
    clients.push_back(subscriber.first);
  }
  return clients;
}

int MeasureServiceImpl::GetThreshold() const {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  return current_threshold_;
}

void MeasureServiceImpl::SetThreshold(int threshold) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  event_store_.RecordEvent("threshold", CurrentTimeMillis(), "",
                           "{\"mode\":\"NORMAL\"}", std::nullopt, threshold,
                           "NORMAL");
  current_threshold_ = threshold;
  Command cmd;
  cmd.set_threshold(threshold);
  cmd.set_mode(Mode::NORMAL);
  for (auto& subscriber : subscribers_) {
    subscriber.second->PushCommand(cmd);
  }
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
  try {
    event_store_.RecordMeasurement(measurement);
  } catch (const std::exception& error) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
    return reactor;
  }
  reply->set_response(0);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(Status::OK);
  return reactor;
}

grpc::ServerWriteReactor<Command>* MeasureServiceImpl::Subscribe(
    grpc::CallbackServerContext* context, const ClientInfo* request) {
  return new SubscribeReactor(request->client_id(), GetThreshold(), this);
}

grpc::ServerUnaryReactor* MeasureServiceImpl::SetCalibrationMode(
    grpc::CallbackServerContext* context, const CalibrationRequest* request,
    CalibrationResponse* reply) {
  try {
    StartCalibration(request->client_id(), request->duration_seconds());
  } catch (const std::exception& error) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
    return reactor;
  }
  reply->set_accepted(true);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(Status::OK);
  return reactor;
}

void MeasureServiceImpl::StartCalibration(const std::string& client_id,
                                          int duration_seconds) {
  std::lock_guard<std::mutex> lock(calibration_mu_);
  if (shutting_down_) return;

  Command cal_cmd;
  cal_cmd.set_threshold(0);
  cal_cmd.set_mode(Mode::CALIBRATION);
  const int duration = duration_seconds > 0 ? duration_seconds : 10;
  event_store_.RecordEvent(
      "command", CurrentTimeMillis(), client_id,
      "{\"action\":\"calibration_started\",\"duration_seconds\":" +
          std::to_string(duration) + ",\"mode\":\"CALIBRATION\"}",
      std::nullopt, 0, "CALIBRATION");

  if (!client_id.empty()) {
    SendCommandToClient(client_id, cal_cmd);
  } else {
    BroadcastCommand(cal_cmd);
  }
  std::cout << "Calibration started"
            << (client_id.empty()
                    ? " (all clients)"
                    : " (client: " + client_id + ")")
            << " for " << duration_seconds << "s" << std::endl;

  calibration_threads_.emplace_back([this, duration, client_id]() {
    std::unique_lock<std::mutex> lock(calibration_mu_);
    if (calibration_cv_.wait_for(lock, std::chrono::seconds(duration),
                                 [this]() { return shutting_down_; })) {
      return;
    }
    lock.unlock();
    Command normal_cmd;
    normal_cmd.set_threshold(GetThreshold());
    normal_cmd.set_mode(Mode::NORMAL);
    try {
      event_store_.RecordEvent(
          "command", CurrentTimeMillis(), client_id,
          "{\"action\":\"calibration_ended\",\"mode\":\"NORMAL\"}",
          std::nullopt, normal_cmd.threshold(), "NORMAL");
    } catch (const std::exception& error) {
      std::cerr << "Failed to store calibration command: " << error.what()
                << std::endl;
    }
    if (client_id.empty()) {
      BroadcastCommand(normal_cmd);
    } else {
      SendCommandToClient(client_id, normal_cmd);
    }
    std::cout << "Calibration ended, threshold restored to "
              << normal_cmd.threshold() << std::endl;
  });
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
