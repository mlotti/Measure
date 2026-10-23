#include "measure_client.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <utility>

namespace {

int64_t CurrentTimeMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool IsRetryable(const grpc::Status& status) {
  return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
         status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
         status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED ||
         status.error_code() == grpc::StatusCode::ABORTED ||
         status.error_code() == grpc::StatusCode::CANCELLED;
}

std::string SessionId() {
  std::random_device random;
  std::ostringstream id;
  id << std::hex;
  for (int i = 0; i < 4; ++i) id << random() << '-';
  return id.str();
}

}

MeasureClient::MeasureClient(std::shared_ptr<grpc::Channel> channel,
                             std::string client_id)
    : stub_(measure::Measure::NewStub(channel)),
      client_id_(std::move(client_id)), session_id_(SessionId()) {}

MeasureClient::~MeasureClient() { Stop(); }

void MeasureClient::Start() {
  subscription_thread_ = std::thread([this] { RunSubscription(); });
  measurement_thread_ = std::thread([this] { RunMeasurements(); });
}

void MeasureClient::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
    if (subscription_context_) subscription_context_->TryCancel();
    if (measurement_context_) measurement_context_->TryCancel();
  }
  cv_.notify_all();
  if (subscription_thread_.joinable()) subscription_thread_.join();
  if (measurement_thread_.joinable()) measurement_thread_.join();
}

int MeasureClient::GetThreshold() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (applied_command_.revision() == 0) return 8;
  if (applied_command_.mode() == measure::CALIBRATION &&
      applied_command_.expires_unix_ms() <= CurrentTimeMillis()) {
    return applied_command_.normal_threshold();
  }
  return applied_command_.threshold();
}

bool MeasureClient::WaitForCommand(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait_for(lock, timeout, [this] {
    return applied_command_.revision() > 0 || failed_ || stopping_;
  });
  return applied_command_.revision() > 0 && !failed_ && !stopping_;
}

bool MeasureClient::RecordMeasurement(int value) {
  std::lock_guard<std::mutex> lock(mu_);
  if (stopping_ || failed_ || pending_.size() >= 1000) {
    std::cerr << "Measurement not queued: client stopped, failed, or the "
                 "1,000-measurement offline buffer is full." << std::endl;
    return false;
  }
  measure::Measurement measurement;
  measurement.set_point(value);
  measurement.set_client_id(client_id_);
  measurement.set_timestamp_unix_ms(CurrentTimeMillis());
  measurement.set_measurement_id(session_id_ + std::to_string(++next_measurement_));
  pending_.push_back(std::move(measurement));
  cv_.notify_all();
  return true;
}

bool MeasureClient::WaitForMeasurements(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait_for(lock, timeout, [this] {
    return pending_.empty() || failed_ || stopping_;
  });
  return pending_.empty() && !failed_;
}

size_t MeasureClient::PendingMeasurements() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_.size();
}

void MeasureClient::Fail(const std::string& message) {
  std::lock_guard<std::mutex> lock(mu_);
  std::cerr << message << std::endl;
  failed_ = true;
  if (subscription_context_) subscription_context_->TryCancel();
  if (measurement_context_) measurement_context_->TryCancel();
  cv_.notify_all();
}

bool MeasureClient::WaitBeforeRetry(std::chrono::milliseconds delay) {
  std::unique_lock<std::mutex> lock(mu_);
  // Jitter keeps a reconnecting fleet from retrying in lockstep.
  thread_local std::mt19937 random(std::random_device{}());
  const auto jitter = std::chrono::milliseconds(
      std::uniform_int_distribution<int>(0, static_cast<int>(delay.count() / 4))(
          random));
  return !cv_.wait_for(lock, delay + jitter,
                       [this] { return stopping_ || failed_; });
}

void MeasureClient::RunSubscription() {
  auto backoff = std::chrono::milliseconds(100);
  while (true) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                          std::chrono::seconds(30));
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_ || failed_) return;
      subscription_context_ = &context;
    }
    measure::ClientInfo info;
    info.set_client_id(client_id_);
    info.set_protocol_version(1);
    auto reader = stub_->Subscribe(&context, info);
    measure::Command command;
    while (reader->Read(&command)) {
      const bool calibration = command.mode() == measure::CALIBRATION;
      if (command.protocol_version() != 1 || command.revision() <= 0 ||
          (command.mode() != measure::NORMAL && !calibration) ||
          command.threshold() < 0 || command.threshold() > 100000 ||
          command.normal_threshold() < 0 || command.normal_threshold() > 100000 ||
          (calibration ? command.expires_unix_ms() <= 0
                       : command.expires_unix_ms() != 0)) {
        Fail("Unsupported or invalid command protocol; stopping client.");
        break;
      }
      backoff = std::chrono::milliseconds(100);
      bool acknowledge = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_ || failed_) break;
        if (command.revision() < applied_command_.revision()) continue;
        if (command.revision() > applied_command_.revision()) {
          applied_command_ = command;
          std::cout << "Command applied: revision=" << command.revision()
                    << " threshold=" << command.threshold() << std::endl;
          cv_.notify_all();
        }
        acknowledge = !calibration ||
                      command.expires_unix_ms() > CurrentTimeMillis();
      }
      if (!acknowledge) continue;
      measure::CommandAcknowledgment acknowledgment;
      acknowledgment.set_client_id(client_id_);
      acknowledgment.set_revision(command.revision());
      measure::CommandReceipt receipt;
      grpc::ClientContext ack_context;
      ack_context.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::seconds(2));
      const auto status = stub_->AcknowledgeCommand(&ack_context, acknowledgment,
                                                    &receipt);
      if (!status.ok()) {
        if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
          std::cerr << "Command superseded or expired before acknowledgment."
                    << std::endl;
        } else if (IsRetryable(status)) {
          std::cerr << "Command acknowledgment failed; waiting for redelivery: "
                    << status.error_message() << std::endl;
        } else {
          Fail("Command acknowledgment rejected: " + status.error_message());
          break;
        }
      }
    }
    const auto status = reader->Finish();
    {
      std::lock_guard<std::mutex> lock(mu_);
      subscription_context_ = nullptr;
      if (stopping_ || failed_) return;
    }
    if (!status.ok() && !IsRetryable(status)) {
      Fail("Subscription rejected: " + status.error_message());
      return;
    }
    std::cerr << "Subscription disconnected; reconnecting: "
              << status.error_message() << std::endl;
    if (!WaitBeforeRetry(backoff)) return;
    backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
  }
}

void MeasureClient::RunMeasurements() {
  auto backoff = std::chrono::milliseconds(100);
  while (true) {
    measure::Measurement measurement;
    grpc::ClientContext context;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || failed_ || !pending_.empty(); });
      if (stopping_ || failed_) return;
      measurement = pending_.front();
      context.set_deadline(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
      measurement_context_ = &context;
    }
    measure::MeasurementReceipt receipt;
    const auto status = stub_->RecordMeasurement(&context, measurement, &receipt);
    {
      std::lock_guard<std::mutex> lock(mu_);
      measurement_context_ = nullptr;
      if (stopping_ || failed_) return;
    }
    if (status.ok()) {
      if (receipt.measurement_id() != measurement.measurement_id()) {
        Fail("Server did not return a matching measurement receipt; "
             "protocol version 1 is required.");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.pop_front();
      }
      cv_.notify_all();
      std::cout << "Measurement received: " << measurement.measurement_id()
                << (receipt.duplicate() ? " (already stored)" : " (stored)")
                << std::endl;
      backoff = std::chrono::milliseconds(100);
    } else {
      if (!IsRetryable(status)) {
        Fail("Measurement rejected; retained in buffer: " + status.error_message());
        return;
      }
      std::cerr << "Measurement delivery failed; buffered for retry: "
                << status.error_message() << std::endl;
      if (!WaitBeforeRetry(backoff)) return;
      backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
    }
  }
}

bool MeasureClient::SetCalibrationMode(const std::string& target_client_id,
                                       int duration_seconds) {
  measure::CalibrationRequest request;
  request.set_client_id(target_client_id);
  request.set_duration_seconds(duration_seconds);
  measure::CalibrationResponse response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  const auto status = stub_->SetCalibrationMode(&context, request, &response);
  if (!status.ok()) {
    std::cerr << "SetCalibrationMode failed: " << status.error_message()
              << std::endl;
    return false;
  }
  std::cout << "Calibration accepted (device application is acknowledged "
               "separately)." << std::endl;
  return response.accepted();
}
