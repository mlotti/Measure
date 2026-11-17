#include "measure_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
  movement_thread_ = std::thread([this] { RunMovement(); });
}

void MeasureClient::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
    if (subscription_context_) subscription_context_->TryCancel();
    if (measurement_context_) measurement_context_->TryCancel();
    if (position_context_) position_context_->TryCancel();
  }
  cv_.notify_all();
  if (subscription_thread_.joinable()) subscription_thread_.join();
  if (measurement_thread_.joinable()) measurement_thread_.join();
  if (movement_thread_.joinable()) movement_thread_.join();
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

bool MeasureClient::ReportPosition(double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 100 || y < 0 ||
      y > 100) {
    std::cerr << "Position must contain finite coordinates from 0 to 100."
              << std::endl;
    return false;
  }
  measure::PositionReport position;
  position.set_client_id(client_id_);
  position.set_x(x);
  position.set_y(y);
  position.set_timestamp_unix_ms(CurrentTimeMillis());
  measure::PositionReceipt receipt;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(2));
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ || failed_) return false;
    position_context_ = &context;
  }
  const auto status = stub_->ReportPosition(&context, position, &receipt);
  {
    std::lock_guard<std::mutex> lock(mu_);
    position_context_ = nullptr;
    if (stopping_ || failed_) return false;
  }
  if (!status.ok()) {
    std::cerr << "Position report failed: " << status.error_message()
              << std::endl;
    return false;
  }
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

bool MeasureClient::HasFailed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return failed_;
}

void MeasureClient::Fail(const std::string& message) {
  std::lock_guard<std::mutex> lock(mu_);
  std::cerr << message << std::endl;
  failed_ = true;
  if (subscription_context_) subscription_context_->TryCancel();
  if (measurement_context_) measurement_context_->TryCancel();
  if (position_context_) position_context_->TryCancel();
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
  uint32_t protocol_version = 2;
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
    info.set_protocol_version(protocol_version);
    auto reader = stub_->Subscribe(&context, info);
    measure::Command command;
    while (reader->Read(&command)) {
      const bool calibration = command.mode() == measure::CALIBRATION;
      const bool movement_protocol = command.protocol_version() == 2;
      if (command.protocol_version() != protocol_version ||
          command.revision() <= 0 ||
          (command.mode() != measure::NORMAL && !calibration) ||
          command.threshold() < 0 || command.threshold() > 100000 ||
          command.normal_threshold() < 0 || command.normal_threshold() > 100000 ||
          (calibration ? command.expires_unix_ms() <= 0
                       : command.expires_unix_ms() != 0) ||
          (movement_protocol && command.has_destination() &&
           (!std::isfinite(command.destination_x()) ||
            !std::isfinite(command.destination_y()) ||
            !std::isfinite(command.movement_speed()) ||
            command.destination_x() < 0 || command.destination_x() > 100 ||
            command.destination_y() < 0 || command.destination_y() > 100 ||
            command.movement_speed() <= 0))) {
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
        }
        if (movement_protocol) {
          movement_enabled_ = true;
          position_report_pending_ = command.has_destination();
          position_retry_at_ = std::chrono::steady_clock::now();
        }
        cv_.notify_all();
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
      movement_enabled_ = false;
    }
    if (protocol_version == 2 &&
        status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
      std::cerr << "Movement protocol unavailable; using threshold-only protocol."
                << std::endl;
      protocol_version = 1;
      continue;
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

void MeasureClient::RunMovement() {
  auto last_update = std::chrono::steady_clock::now();
  auto next_report = last_update;
  while (true) {
    double report_x = 0;
    double report_y = 0;
    bool report = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock, std::chrono::milliseconds(100),
                   [this] {
                     return stopping_ || failed_ ||
                            (position_report_pending_ &&
                             std::chrono::steady_clock::now() >=
                                 position_retry_at_);
                   });
      if (stopping_ || failed_) return;

      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration<double>(now - last_update).count();
      last_update = now;
      const auto& command = applied_command_;
      const bool commanded_to_move =
          movement_enabled_ && command.protocol_version() == 2 &&
          command.has_destination();
      const bool report_before_moving = position_report_pending_;
      bool moving = false;
      if (commanded_to_move && !report_before_moving) {
        const double dx = command.destination_x() - position_x_;
        const double dy = command.destination_y() - position_y_;
        const double distance = std::hypot(dx, dy);
        const double step = command.movement_speed() * elapsed;
        if (distance > 0 && distance <= step) {
          position_x_ = command.destination_x();
          position_y_ = command.destination_y();
          report = true;
        } else if (distance > 0) {
          position_x_ += dx * step / distance;
          position_y_ += dy * step / distance;
          moving = true;
        }
        position_x_ = std::clamp(position_x_, 0.0, 100.0);
        position_y_ = std::clamp(position_y_, 0.0, 100.0);
      }
      if (report_before_moving || (moving && now >= next_report)) {
        report = true;
        position_report_pending_ = false;
      }
      if (report) {
        report_x = position_x_;
        report_y = position_y_;
        next_report = now + std::chrono::seconds(1);
      }
    }
    if (report && !ReportPosition(report_x, report_y)) {
      std::lock_guard<std::mutex> lock(mu_);
      if (!stopping_ && !failed_) {
        position_report_pending_ = true;
        position_retry_at_ = std::chrono::steady_clock::now() +
                             std::chrono::seconds(1);
      }
    }
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
