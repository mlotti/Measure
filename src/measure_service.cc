#include "measure_service.h"
#include "device_identity.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

using grpc::Status;
using measure::Command;

namespace {

int64_t CurrentTimeMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string PeerIdentity(const grpc::ServerContextBase& context) {
  const auto auth_context = context.auth_context();
  if (!auth_context) return "";
  const auto common_names = auth_context->FindPropertyValues("x509_common_name");
  if (common_names.size() != 1) return "";
  return std::string(common_names.front().data(), common_names.front().size());
}

}

class SubscribeReactor : public grpc::ServerWriteReactor<Command> {
 public:
  SubscribeReactor(std::string client_id, uint32_t protocol_version,
                   MeasureServiceImpl* service)
      : client_id_(std::move(client_id)),
        protocol_version_(protocol_version),
        service_(service) {
    try {
      service_->AddSubscriber(client_id_, this);
    } catch (const std::exception& error) {
      finished_ = true;
      Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
    }
  }

  bool reliable() const { return protocol_version_ >= 1; }
  uint32_t protocol_version() const { return protocol_version_; }

  void PushCommand(const Command& cmd) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ || finished_) return;
    // Only the current write and newest pending snapshot are retained.
    if (writing_) {
      if (cmd.revision() > current_write_.revision()) pending_ = cmd;
    } else {
      current_write_ = cmd;
      writing_ = true;
      StartWrite(&current_write_);
    }
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
    pending_.reset();
    if (!writing_) FinishOnce();
  }

  void OnWriteDone(bool ok) override {
    std::lock_guard<std::mutex> lock(mu_);
    writing_ = false;
    if (!ok || stopping_) {
      FinishOnce();
    } else if (pending_) {
      current_write_ = *pending_;
      pending_.reset();
      writing_ = true;
      StartWrite(&current_write_);
    }
  }

  void OnCancel() override { Stop(); }

  void OnDone() override {
    service_->RemoveSubscriber(client_id_, this);
    delete this;
  }

 private:
  void FinishOnce() {
    if (finished_) return;
    finished_ = true;
    Finish(Status::CANCELLED);
  }

  std::string client_id_;
  uint32_t protocol_version_;
  MeasureServiceImpl* service_;
  std::mutex mu_;
  std::optional<Command> pending_;
  Command current_write_;
  bool writing_ = false;
  bool stopping_ = false;
  bool finished_ = false;
};

class RejectedSubscribeReactor : public grpc::ServerWriteReactor<Command> {
 public:
  explicit RejectedSubscribeReactor(const Status& status) { Finish(status); }
  void OnDone() override { delete this; }
};

MeasureServiceImpl::MeasureServiceImpl(size_t max_measurements,
                                       const std::string& database_path,
                                       int retention_days,
                                       size_t max_concurrent_measurements)
    : event_store_(database_path, retention_days),
      max_measurements_(max_measurements),
      max_concurrent_measurements_(max_concurrent_measurements) {
  if (max_concurrent_measurements_ == 0) {
    throw std::invalid_argument("Measurement concurrency limit must be positive.");
  }
  event_store_.ExpireCalibrations(CurrentTimeMillis());
  try {
    for (size_t i = 0; i < max_concurrent_measurements_; ++i) {
      ingestion_threads_.emplace_back([this] { RunIngestion(); });
    }
    delivery_thread_ = std::thread([this] { RunDelivery(); });
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(ingestion_mu_);
      ingestion_stopping_ = true;
    }
    ingestion_cv_.notify_all();
    for (auto& thread : ingestion_threads_) thread.join();
    throw;
  }
}

MeasureServiceImpl::~MeasureServiceImpl() {
  {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    shutting_down_ = true;
  }
  delivery_cv_.notify_all();
  delivery_thread_.join();
  {
    std::lock_guard<std::mutex> lock(ingestion_mu_);
    ingestion_stopping_ = true;
  }
  ingestion_cv_.notify_all();
  for (auto& thread : ingestion_threads_) thread.join();
}

void MeasureServiceImpl::RunIngestion() {
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(ingestion_mu_);
      ingestion_cv_.wait(lock, [this] {
        return ingestion_stopping_ || !ingestion_jobs_.empty();
      });
      if (ingestion_jobs_.empty()) return;
      job = std::move(ingestion_jobs_.front());
      ingestion_jobs_.pop();
    }
    job();
  }
}

measure::Command MeasureServiceImpl::MakeCommand(
    const std::string& client_id, uint32_t protocol_version) const {
  const auto desired = event_store_.GetDesiredCommand(client_id);
  Command command;
  command.set_threshold(desired.threshold);
  command.set_mode(desired.expires_unix_ms > 0 ? measure::CALIBRATION
                                               : measure::NORMAL);
  command.set_revision(desired.revision);
  command.set_expires_unix_ms(desired.expires_unix_ms);
  command.set_protocol_version(protocol_version);
  command.set_normal_threshold(event_store_.GetLatestThreshold(8));
  if (protocol_version >= 2) {
    const auto movement = event_store_.GetDesiredMovement(client_id);
    command.set_has_destination(movement.has_destination);
    command.set_destination_x(movement.destination_x);
    command.set_destination_y(movement.destination_y);
    command.set_movement_speed(movement.speed);
  }
  return command;
}

void MeasureServiceImpl::AddSubscriber(const std::string& client_id,
                                       SubscribeReactor* reactor) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  event_store_.ExpireCalibrations(CurrentTimeMillis());
  event_store_.RegisterDevice(client_id);
  const auto command = MakeCommand(client_id, reactor->protocol_version());
  const auto old = subscribers_.find(client_id);
  if (old != subscribers_.end()) old->second->Stop();
  subscribers_[client_id] = reactor;
  reactor->PushCommand(command);
  std::cout << "Client subscribed: " << client_id << std::endl;
}

void MeasureServiceImpl::RemoveSubscriber(const std::string& client_id,
                                          SubscribeReactor* reactor) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  const auto it = subscribers_.find(client_id);
  if (it != subscribers_.end() && it->second == reactor) {
    subscribers_.erase(it);
    std::cout << "Client unsubscribed: " << client_id << std::endl;
  }
}

std::vector<measure::StoredMeasurement>
MeasureServiceImpl::GetMeasurementsSnapshot() const {
  return event_store_.GetMeasurements(max_measurements_);
}

std::vector<measure::StoredMeasurement>
MeasureServiceImpl::GetLatestMeasurementsSnapshot() const {
  return event_store_.GetLatestMeasurements();
}

std::vector<measure::StoredPosition>
MeasureServiceImpl::GetLatestPositionsSnapshot() const {
  return event_store_.GetLatestPositions();
}

std::vector<measure::DesiredMovement> MeasureServiceImpl::GetDesiredMovements() const {
  return event_store_.GetDesiredMovements();
}

std::vector<measure::StoredEvent> MeasureServiceImpl::GetEventsSnapshot(
    size_t limit, int64_t since_unix_ms, int64_t until_unix_ms,
    const std::string& client_id) const {
  return event_store_.GetEvents(limit, since_unix_ms, until_unix_ms, client_id);
}

std::vector<measure::MeasurementTrend> MeasureServiceImpl::GetTrends(
    int64_t bucket_ms, int64_t since_unix_ms, int64_t until_unix_ms,
    const std::string& client_id) const {
  return event_store_.GetTrends(bucket_ms, since_unix_ms, until_unix_ms, client_id);
}

std::vector<std::string> MeasureServiceImpl::GetConnectedClients() const {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  std::vector<std::string> clients;
  for (const auto& subscriber : subscribers_) clients.push_back(subscriber.first);
  return clients;
}

std::vector<measure::CommandDelivery>
MeasureServiceImpl::GetCommandDeliveries() const {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  return event_store_.GetCommandDeliveries();
}

int MeasureServiceImpl::GetThreshold() const {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  return event_store_.GetLatestThreshold(8);
}

void MeasureServiceImpl::PublishDesiredCommands(bool only_pending) {
  const auto deliveries = event_store_.GetCommandDeliveries();
  for (const auto& delivery : deliveries) {
    const auto it = subscribers_.find(delivery.client_id);
    if (it == subscribers_.end()) continue;
    if (only_pending &&
        (!it->second->reliable() ||
         delivery.desired_revision == delivery.acknowledged_revision)) {
      continue;
    }
    it->second->PushCommand(
        MakeCommand(delivery.client_id, it->second->protocol_version()));
  }
}

void MeasureServiceImpl::RunDelivery() {
  std::unique_lock<std::mutex> lock(subscribers_mu_);
  auto next_retry = std::chrono::steady_clock::now();
  while (!delivery_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                [this] { return shutting_down_; })) {
    try {
      const bool expired = event_store_.ExpireCalibrations(CurrentTimeMillis());
      const auto now = std::chrono::steady_clock::now();
      if (expired || now >= next_retry) {
        PublishDesiredCommands(!expired);
        next_retry = now + std::chrono::seconds(1);
      }
    } catch (const std::exception& error) {
      std::cerr << "Command delivery failed; will retry: " << error.what()
                << std::endl;
    }
  }
}

void MeasureServiceImpl::SetThreshold(int threshold,
                                      const std::string& actor_identity) {
  if (threshold < 0 || threshold > 100000) {
    throw std::invalid_argument("Threshold must be from 0 to 100000.");
  }
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  event_store_.SetDesiredThreshold(threshold, actor_identity);
  PublishDesiredCommands();
}

grpc::ServerUnaryReactor* MeasureServiceImpl::RecordMeasurement(
    grpc::CallbackServerContext* context, const measure::Measurement* request,
    measure::MeasurementReceipt* reply) {
  auto* reactor = context->DefaultReactor();
  if (!measure::IsDeviceIdentityForClient(PeerIdentity(*context),
                                          request->client_id())) {
    reactor->Finish(Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Device certificate does not match client_id."));
    return reactor;
  }
  if (request->measurement_id().size() > 128 ||
      (!request->measurement_id().empty() && request->timestamp_unix_ms() <= 0)) {
    reactor->Finish(Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Retryable measurements require a timestamp and an "
                           "ID of at most 128 bytes."));
    return reactor;
  }
  std::lock_guard<std::mutex> lock(ingestion_mu_);
  if (ingestion_stopping_ ||
      active_measurements_ >= max_concurrent_measurements_) {
    reactor->Finish(Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                           "Measurement ingestion is full; retry with backoff."));
    return reactor;
  }
  ingestion_jobs_.push([this, request, reply, reactor] {
    Status status;
    try {
      const bool inserted = event_store_.RecordMeasurement(
          {request->client_id(), request->point(),
           request->timestamp_unix_ms() > 0 ? request->timestamp_unix_ms()
                                            : CurrentTimeMillis(),
           request->measurement_id()});
      reply->set_duplicate(!inserted);
      reply->set_measurement_id(request->measurement_id());
    } catch (const std::invalid_argument& error) {
      status = Status(grpc::StatusCode::ALREADY_EXISTS, error.what());
    } catch (const std::exception& error) {
      status = Status(grpc::StatusCode::INTERNAL, error.what());
    }
    {
      std::lock_guard<std::mutex> lock(ingestion_mu_);
      --active_measurements_;
    }
    reactor->Finish(status);
  });
  ++active_measurements_;
  ingestion_cv_.notify_one();
  return reactor;
}

grpc::ServerWriteReactor<Command>* MeasureServiceImpl::Subscribe(
    grpc::CallbackServerContext* context, const measure::ClientInfo* request) {
  if (!measure::IsDeviceIdentityForClient(PeerIdentity(*context),
                                          request->client_id())) {
    return new RejectedSubscribeReactor(
        Status(grpc::StatusCode::UNAUTHENTICATED,
               "Device certificate does not match client_id."));
  }
  if (request->protocol_version() > 2) {
    return new RejectedSubscribeReactor(
        Status(grpc::StatusCode::FAILED_PRECONDITION,
               "Supported subscription protocol versions are 0, 1, and 2."));
  }
  return new SubscribeReactor(request->client_id(), request->protocol_version(),
                               this);
}

grpc::ServerUnaryReactor* MeasureServiceImpl::AcknowledgeCommand(
    grpc::CallbackServerContext* context,
    const measure::CommandAcknowledgment* request,
    measure::CommandReceipt*) {
  auto* reactor = context->DefaultReactor();
  if (!measure::IsDeviceIdentityForClient(PeerIdentity(*context),
                                          request->client_id())) {
    reactor->Finish(Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Device certificate does not match client_id."));
    return reactor;
  }
  try {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    if (request->revision() <= 0 ||
        !event_store_.AcknowledgeCommand(request->client_id(),
                                         request->revision())) {
      reactor->Finish(Status(grpc::StatusCode::FAILED_PRECONDITION,
                             "Command is superseded, expired, or unknown."));
    } else {
      reactor->Finish(Status::OK);
    }
  } catch (const std::exception& error) {
    reactor->Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
  }
  return reactor;
}

grpc::ServerUnaryReactor* MeasureServiceImpl::ReportPosition(
    grpc::CallbackServerContext* context, const measure::PositionReport* request,
    measure::PositionReceipt*) {
  auto* reactor = context->DefaultReactor();
  if (!measure::IsDeviceIdentityForClient(PeerIdentity(*context),
                                          request->client_id())) {
    reactor->Finish(Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Device certificate does not match client_id."));
    return reactor;
  }
  if (!std::isfinite(request->x()) || !std::isfinite(request->y()) ||
      request->x() < 0 || request->x() > 100 || request->y() < 0 ||
      request->y() > 100 || request->timestamp_unix_ms() <= 0) {
    reactor->Finish(Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Position coordinates must be finite values from 0 to 100 "
                           "with a positive timestamp."));
    return reactor;
  }
  try {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    const auto subscriber = subscribers_.find(request->client_id());
    if (subscriber == subscribers_.end() ||
        subscriber->second->protocol_version() < 2) {
      reactor->Finish(Status(grpc::StatusCode::FAILED_PRECONDITION,
                             "Position reports require protocol version 2."));
      return reactor;
    }
    event_store_.RecordPosition(
        {request->client_id(), request->x(), request->y(),
         request->timestamp_unix_ms()});
    reactor->Finish(Status::OK);
  } catch (const std::exception& error) {
    reactor->Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
  }
  return reactor;
}

grpc::ServerUnaryReactor* MeasureServiceImpl::SetCalibrationMode(
    grpc::CallbackServerContext* context,
    const measure::CalibrationRequest* request,
    measure::CalibrationResponse* reply) {
  auto* reactor = context->DefaultReactor();
  const std::string actor_identity = PeerIdentity(*context);
  if (!measure::IsOperatorIdentity(actor_identity)) {
    reactor->Finish(Status(grpc::StatusCode::PERMISSION_DENIED,
                           "An operator certificate is required."));
    return reactor;
  }
  try {
    StartCalibration(request->client_id(), request->duration_seconds(),
                     actor_identity);
    reply->set_accepted(true);
    reactor->Finish(Status::OK);
  } catch (const std::invalid_argument& error) {
    reactor->Finish(Status(grpc::StatusCode::INVALID_ARGUMENT, error.what()));
  } catch (const std::exception& error) {
    reactor->Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
  }
  return reactor;
}

void MeasureServiceImpl::StartCalibration(const std::string& client_id,
                                          int duration_seconds,
                                          const std::string& actor_identity) {
  const int duration = duration_seconds == 0 ? 10 : duration_seconds;
  if (duration < 1 || duration > 3600) {
    throw std::invalid_argument("Calibration duration must be from 1 to 3600.");
  }
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  std::vector<std::string> clients;
  if (client_id.empty()) {
    for (const auto& subscriber : subscribers_) clients.push_back(subscriber.first);
    if (clients.empty()) {
      throw std::invalid_argument("No connected devices to calibrate.");
    }
  } else {
    event_store_.RegisterDevice(client_id);
    clients.push_back(client_id);
  }
  event_store_.SetDesiredCalibration(
      clients, CurrentTimeMillis() + static_cast<int64_t>(duration) * 1000,
      actor_identity);
  PublishDesiredCommands();
}

void MeasureServiceImpl::SetDestination(const std::string& client_id,
                                        double destination_x,
                                        double destination_y, double speed,
                                        const std::string& actor_identity) {
  if (client_id.empty()) {
    throw std::invalid_argument("A destination requires a device ID.");
  }
  if (!std::isfinite(destination_x) || !std::isfinite(destination_y) ||
      destination_x < 0 || destination_x > 100 || destination_y < 0 ||
      destination_y > 100 || !std::isfinite(speed) || speed <= 0) {
    throw std::invalid_argument(
        "Destination coordinates must be finite values from 0 to 100 and "
        "speed must be finite and positive.");
  }
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  event_store_.RegisterDevice(client_id);
  event_store_.SetDesiredMovement(client_id, true, destination_x, destination_y,
                                  speed, actor_identity);
  PublishDesiredCommands();
}
