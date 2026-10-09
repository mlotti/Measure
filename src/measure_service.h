#ifndef MEASURE_MEASURE_SERVICE_H
#define MEASURE_MEASURE_SERVICE_H

#include <cstddef>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "measure.grpc.pb.h"
#include "event_store.h"
#include "measurement_utils.h"

class SubscribeReactor;

class MeasureServiceImpl final : public measure::Measure::CallbackService {
 public:
  explicit MeasureServiceImpl(size_t max_measurements,
                              const std::string& database_path = ":memory:",
                              int retention_days = 0);
  ~MeasureServiceImpl();

  void AddSubscriber(const std::string& client_id, SubscribeReactor* reactor);
  void RemoveSubscriber(const std::string& client_id);
  std::vector<measure::StoredMeasurement> GetMeasurementsSnapshot() const;
  std::vector<measure::StoredEvent> GetEventsSnapshot(
      size_t limit, int64_t since_unix_ms = 0, int64_t until_unix_ms = 0,
      const std::string& client_id = "") const;
  std::vector<measure::MeasurementTrend> GetTrends(
      int64_t bucket_ms, int64_t since_unix_ms = 0,
      int64_t until_unix_ms = 0, const std::string& client_id = "") const;
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
  measure::EventStore event_store_;
  size_t max_measurements_;
  int current_threshold_ = 8;
  std::mutex calibration_mu_;
  std::condition_variable calibration_cv_;
  bool shutting_down_ = false;
  std::vector<std::thread> calibration_threads_;
};

#endif
