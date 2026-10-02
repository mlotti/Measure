#ifndef MEASURE_MEASURE_SERVICE_H
#define MEASURE_MEASURE_SERVICE_H

#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "measure.grpc.pb.h"
#include "measurement_utils.h"

class SubscribeReactor;

class MeasureServiceImpl final : public measure::Measure::CallbackService {
 public:
  explicit MeasureServiceImpl(size_t max_measurements);

  void AddSubscriber(const std::string& client_id, SubscribeReactor* reactor);
  void RemoveSubscriber(const std::string& client_id);
  std::vector<measure::StoredMeasurement> GetMeasurementsSnapshot() const;

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

  std::mutex subscribers_mu_;
  std::map<std::string, SubscribeReactor*> subscribers_;
  int current_threshold_ = 8;
  size_t max_measurements_;
  mutable std::mutex measurements_mu_;
  std::deque<measure::StoredMeasurement> measurements_;
};

#endif
