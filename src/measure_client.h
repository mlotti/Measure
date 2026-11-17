#ifndef MEASURE_MEASURE_CLIENT_H
#define MEASURE_MEASURE_CLIENT_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include "measure.grpc.pb.h"

class MeasureClient {
 public:
  MeasureClient(std::shared_ptr<grpc::Channel> channel, std::string client_id);
  ~MeasureClient();

  void Start();
  void Stop();
  int GetThreshold() const;
  bool WaitForCommand(std::chrono::milliseconds timeout);
  bool RecordMeasurement(int value);
  bool ReportPosition(double x, double y);
  bool WaitForMeasurements(std::chrono::milliseconds timeout);
  size_t PendingMeasurements() const;
  bool HasFailed() const;
  bool SetCalibrationMode(const std::string& target_client_id,
                          int duration_seconds);

 private:
  void RunSubscription();
  void RunMeasurements();
  void RunMovement();
  void RunPositionReports();
  void QueuePositionReportLocked(double x, double y);
  bool WaitBeforeRetry(std::chrono::milliseconds delay);
  void Fail(const std::string& message);

  std::unique_ptr<measure::Measure::Stub> stub_;
  std::string client_id_;
  std::string session_id_;
  uint64_t next_measurement_ = 0;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  bool failed_ = false;
  measure::Command applied_command_;
  double position_x_ = 0;
  double position_y_ = 0;
  bool position_report_pending_ = false;
  double pending_position_x_ = 0;
  double pending_position_y_ = 0;
  std::chrono::steady_clock::time_point position_retry_at_{};
  uint64_t position_sync_generation_ = 0;
  uint64_t pending_position_sync_generation_ = 0;
  bool position_sync_pending_ = false;
  bool movement_enabled_ = false;
  std::deque<measure::Measurement> pending_;
  grpc::ClientContext* subscription_context_ = nullptr;
  grpc::ClientContext* measurement_context_ = nullptr;
  grpc::ClientContext* position_context_ = nullptr;
  std::thread subscription_thread_;
  std::thread measurement_thread_;
  std::thread movement_thread_;
  std::thread position_report_thread_;
};

#endif
