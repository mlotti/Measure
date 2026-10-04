#ifndef MEASURE_MEASURE_SERVICE_H
#define MEASURE_MEASURE_SERVICE_H

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "measure.grpc.pb.h"
#include "measurement_utils.h"

class SubscribeReactor;

class MeasureServiceImpl final : public measure::Measure::CallbackService {
 public:
  explicit MeasureServiceImpl(size_t max_measurements);
  ~MeasureServiceImpl();

  void AddSubscriber(const std::string& client_id, SubscribeReactor* reactor);
  void RemoveSubscriber(const std::string& client_id);
  std::vector<measure::StoredMeasurement> GetMeasurementsSnapshot() const;
  std::vector<std::string> GetConnectedClients() const;
  int GetThreshold() const;
  void SetThreshold(int threshold);
  void StartCalibration(const std::string& client_id, int duration_seconds);

 private:
  grpc::ServerUnaryReactor* RecordMeasurement(
      grpc::CallbackServerContext* context, const measure::Measurement* request,
      measure::Thumbs* reply) override;
  grpc::ServerWriteReactor<measure::Command>* Subscribe(
      grpc::CallbackServerContext* context,
      const measure::ClientInfo* request) override;
  grpc::ServerUnaryReactor* SetCalibrationMode(
      grpc::CallbackServerContext* context,
      const measure::CalibrationRequest* request,
      measure::CalibrationResponse* reply) override;

  void BroadcastCommand(const measure::Command& cmd);
  void SendCommandToClient(const std::string& client_id,
                           const measure::Command& cmd);

  mutable std::mutex subscribers_mu_;
  std::map<std::string, SubscribeReactor*> subscribers_;
  int current_threshold_ = 8;
  size_t max_measurements_;
  mutable std::mutex measurements_mu_;
  std::deque<measure::StoredMeasurement> measurements_;
  std::mutex calibration_mu_;
  std::condition_variable calibration_cv_;
  bool shutting_down_ = false;
  std::vector<std::thread> calibration_threads_;
};

#endif
