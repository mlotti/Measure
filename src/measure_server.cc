#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
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
ABSL_FLAG(std::string, http_bind_address, "127.0.0.1",
          "IPv4 address for the dashboard listener");
ABSL_FLAG(int, samples_retained, 500,
          "Maximum number of recent measurements retained by the dashboard");
ABSL_FLAG(int, max_concurrent_measurements, 4,
          "Maximum concurrent measurement writes; excess requests must retry");
ABSL_FLAG(std::string, database_path, "measure.db",
          "Path to the SQLite event database");
ABSL_FLAG(int, retention_days, 0,
          "Delete events older than this many days; 0 disables expiration");
ABSL_FLAG(std::string, web_root, "dashboard/dist",
          "Directory containing the built dashboard frontend");
ABSL_FLAG(std::string, server_cert, "", "PEM server certificate chain");
ABSL_FLAG(std::string, server_key, "", "PEM server private key");
ABSL_FLAG(std::string, client_ca, "",
          "PEM CA certificate for device and operator certificates");

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to read credential file: " + path);
  std::ostringstream contents;
  contents << input.rdbuf();
  const std::string result = contents.str();
  if (result.empty()) {
    throw std::runtime_error("Credential file is empty: " + path);
  }
  return result;
}

void RunServer(uint16_t port, uint16_t http_port, size_t max_measurements,
               size_t max_concurrent_measurements,
               const std::string& database_path, int retention_days,
               const std::string& web_root, const std::string& http_bind_address,
               const std::string& server_cert,
               const std::string& server_key, const std::string& client_ca) {
  const std::string server_address = "0.0.0.0:" + std::to_string(port);
  MeasureServiceImpl service(max_measurements, database_path, retention_days,
                             max_concurrent_measurements);
  DashboardServer dashboard_server(http_port, service, web_root,
                                   http_bind_address);

  grpc::SslServerCredentialsOptions tls_options;
  tls_options.pem_root_certs = ReadFile(client_ca);
  tls_options.pem_key_cert_pairs.push_back(
      {ReadFile(server_key), ReadFile(server_cert)});
  tls_options.client_certificate_request =
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(server_address,
                           grpc::SslServerCredentials(tls_options));
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  dashboard_server.Start();

  server->Wait();
  dashboard_server.Stop();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  const int concurrency = absl::GetFlag(FLAGS_max_concurrent_measurements);
  if (concurrency < 1) {
    std::cerr << "--max_concurrent_measurements must be positive." << std::endl;
    return 1;
  }
  RunServer(absl::GetFlag(FLAGS_port), absl::GetFlag(FLAGS_http_port),
            static_cast<size_t>(
                std::max(1, absl::GetFlag(FLAGS_samples_retained))),
            static_cast<size_t>(concurrency),
            absl::GetFlag(FLAGS_database_path),
            absl::GetFlag(FLAGS_retention_days),
            absl::GetFlag(FLAGS_web_root),
            absl::GetFlag(FLAGS_http_bind_address),
            absl::GetFlag(FLAGS_server_cert),
            absl::GetFlag(FLAGS_server_key),
            absl::GetFlag(FLAGS_client_ca));
  return 0;
}
