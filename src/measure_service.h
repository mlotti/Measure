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

class SubscribeReactor;

class MeasureServiceImpl final : public measure::Measure::CallbackService {
 public:
  explicit MeasureServiceImpl(size_t max_measurements,
                              const std::string& database_path = ":memory:",
                              int retention_days = 0);
  ~MeasureServiceImpl();

  void AddSubscriber(const std::string& client_id, SubscribeReactor* reactor);
  void RemoveSubscriber(const std::string& client_id, SubscribeReactor* reactor);
  std::vector<measure::StoredMeasurement> GetMeasurementsSnapshot() const;
  std::vector<measure::StoredMeasurement> GetLatestMeasurementsSnapshot() const;
  std::vector<measure::StoredEvent> GetEventsSnapshot(
      size_t limit, int64_t since_unix_ms = 0, int64_t until_unix_ms = 0,
      const std::string& client_id = "") const;
  std::vector<measure::MeasurementTrend> GetTrends(
      int64_t bucket_ms, int64_t since_unix_ms = 0,
      int64_t until_unix_ms = 0, const std::string& client_id = "") const;
  std::vector<std::string> GetConnectedClients() const;
  std::vector<measure::CommandDelivery> GetCommandDeliveries() const;
  int GetThreshold() const;
  void SetThreshold(int threshold,
                    const std::string& actor_identity = "localhost_dashboard");
  void StartCalibration(
      const std::string& client_id, int duration_seconds,
      const std::string& actor_identity = "localhost_dashboard");

 private:
  grpc::ServerUnaryReactor* RecordMeasurement(
      grpc::CallbackServerContext* context, const measure::Measurement* request,
      measure::MeasurementReceipt* reply) override;
  grpc::ServerWriteReactor<measure::Command>* Subscribe(
      grpc::CallbackServerContext* context,
      const measure::ClientInfo* request) override;
  grpc::ServerUnaryReactor* SetCalibrationMode(
      grpc::CallbackServerContext* context,
      const measure::CalibrationRequest* request,
      measure::CalibrationResponse* reply) override;
  grpc::ServerUnaryReactor* AcknowledgeCommand(
      grpc::CallbackServerContext* context,
      const measure::CommandAcknowledgment* request,
      measure::CommandReceipt* reply) override;

  void PublishDesiredCommands(bool only_pending = false);
  measure::Command MakeCommand(const std::string& client_id) const;
  void RunDelivery();

  mutable std::mutex subscribers_mu_;
  std::map<std::string, SubscribeReactor*> subscribers_;
  measure::EventStore event_store_;
  size_t max_measurements_;
  std::condition_variable delivery_cv_;
  bool shutting_down_ = false;
  std::thread delivery_thread_;
};

#endif
