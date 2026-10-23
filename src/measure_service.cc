#include "measure_service.h"
#include "device_identity.h"

#include <chrono>
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
  SubscribeReactor(std::string client_id, bool reliable,
                   MeasureServiceImpl* service)
      : client_id_(std::move(client_id)), reliable_(reliable), service_(service) {
    try {
      service_->AddSubscriber(client_id_, this);
    } catch (const std::exception& error) {
      finished_ = true;
      Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
    }
  }

  bool reliable() const { return reliable_; }

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
  bool reliable_;
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
                                       int retention_days)
    : event_store_(database_path, retention_days),
      max_measurements_(max_measurements) {
  event_store_.ExpireCalibrations(CurrentTimeMillis());
  delivery_thread_ = std::thread([this] { RunDelivery(); });
}

MeasureServiceImpl::~MeasureServiceImpl() {
  {
    std::lock_guard<std::mutex> lock(subscribers_mu_);
    shutting_down_ = true;
  }
  delivery_cv_.notify_all();
  delivery_thread_.join();
}

measure::Command MeasureServiceImpl::MakeCommand(
    const std::string& client_id) const {
  const auto desired = event_store_.GetDesiredCommand(client_id);
  Command command;
  command.set_threshold(desired.threshold);
  command.set_mode(desired.expires_unix_ms > 0 ? measure::CALIBRATION
                                               : measure::NORMAL);
  command.set_revision(desired.revision);
  command.set_expires_unix_ms(desired.expires_unix_ms);
  command.set_protocol_version(1);
  command.set_normal_threshold(event_store_.GetLatestThreshold(8));
  return command;
}

void MeasureServiceImpl::AddSubscriber(const std::string& client_id,
                                       SubscribeReactor* reactor) {
  std::lock_guard<std::mutex> lock(subscribers_mu_);
  event_store_.ExpireCalibrations(CurrentTimeMillis());
  event_store_.RegisterDevice(client_id);
  const auto command = MakeCommand(client_id);
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
    it->second->PushCommand(MakeCommand(delivery.client_id));
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
  try {
    const bool inserted = event_store_.RecordMeasurement(
        {request->client_id(), request->point(),
         request->timestamp_unix_ms() > 0 ? request->timestamp_unix_ms()
                                          : CurrentTimeMillis(),
         request->measurement_id()});
    reply->set_duplicate(!inserted);
    reply->set_measurement_id(request->measurement_id());
    reactor->Finish(Status::OK);
  } catch (const std::invalid_argument& error) {
    reactor->Finish(Status(grpc::StatusCode::ALREADY_EXISTS, error.what()));
  } catch (const std::exception& error) {
    reactor->Finish(Status(grpc::StatusCode::INTERNAL, error.what()));
  }
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
  if (request->protocol_version() > 1) {
    return new RejectedSubscribeReactor(
        Status(grpc::StatusCode::FAILED_PRECONDITION,
               "Supported subscription protocol versions are 0 and 1."));
  }
  return new SubscribeReactor(request->client_id(),
                               request->protocol_version() == 1, this);
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
