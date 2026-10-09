#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "dashboard_server.h"
#include "measure_service.h"

using grpc::Server;
using grpc::ServerBuilder;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");
ABSL_FLAG(uint16_t, http_port, 8080, "HTTP port for the localhost dashboard");
ABSL_FLAG(int, samples_retained, 500,
          "Maximum number of recent measurements retained by the dashboard");
ABSL_FLAG(std::string, database_path, "measure.db",
          "Path to the SQLite event database");
ABSL_FLAG(int, retention_days, 0,
          "Delete events older than this many days; 0 disables expiration");
ABSL_FLAG(std::string, web_root, "dashboard/dist",
          "Directory containing the built dashboard frontend");

void RunServer(uint16_t port, uint16_t http_port, size_t max_measurements,
               const std::string& database_path, int retention_days,
               const std::string& web_root) {
  const std::string server_address = "0.0.0.0:" + std::to_string(port);
  MeasureServiceImpl service(max_measurements, database_path, retention_days);
  DashboardServer dashboard_server(http_port, service, web_root);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  dashboard_server.Start();

  server->Wait();
  dashboard_server.Stop();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  RunServer(absl::GetFlag(FLAGS_port), absl::GetFlag(FLAGS_http_port),
            static_cast<size_t>(
                std::max(1, absl::GetFlag(FLAGS_samples_retained))),
            absl::GetFlag(FLAGS_database_path),
            absl::GetFlag(FLAGS_retention_days),
            absl::GetFlag(FLAGS_web_root));
  return 0;
}
