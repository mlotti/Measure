#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "measure_client.h"

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");
ABSL_FLAG(std::string, client_id, "client1", "Unique client identifier");
ABSL_FLAG(bool, calibrate, false,
          "Operator mode: send a SetCalibrationMode command to the server");
ABSL_FLAG(int, duration, 10, "Calibration duration in seconds (operator mode)");
ABSL_FLAG(std::string, root_cert, "", "PEM CA certificate for the server");
ABSL_FLAG(std::string, client_cert, "", "PEM device or operator certificate");
ABSL_FLAG(std::string, client_key, "", "PEM device or operator private key");

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to read credential file: " + path);
  std::ostringstream contents;
  contents << input.rdbuf();
  const std::string result = contents.str();
  if (result.empty()) throw std::runtime_error("Credential file is empty: " + path);
  return result;
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  try {
    grpc::SslCredentialsOptions tls_options;
    tls_options.pem_root_certs = ReadFile(absl::GetFlag(FLAGS_root_cert));
    tls_options.pem_cert_chain = ReadFile(absl::GetFlag(FLAGS_client_cert));
    tls_options.pem_private_key = ReadFile(absl::GetFlag(FLAGS_client_key));
    MeasureClient client(
        grpc::CreateChannel(absl::GetFlag(FLAGS_target),
                             grpc::SslCredentials(tls_options)),
        absl::GetFlag(FLAGS_client_id));

    if (absl::GetFlag(FLAGS_calibrate)) {
      return client.SetCalibrationMode("", absl::GetFlag(FLAGS_duration)) ? 0 : 1;
    }
    client.Start();
    if (!client.WaitForCommand(std::chrono::seconds(5))) {
      std::cerr << "No initial command received; refusing to sample with an "
                   "unknown threshold." << std::endl;
      return 1;
    }

    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(0, 10);
    bool queued = true;
    for (int i = 0; i < 10; ++i) {
      const int value = dis(gen);
      const int threshold = client.GetThreshold();
      std::cout << "Generated random number: " << value << std::endl;
      if (value > threshold) {
        queued = client.RecordMeasurement(value) && queued;
        std::cout << "Measured: " << value << " (threshold=" << threshold << ")"
                  << std::endl;
      } else {
        std::cout << "Value " << value << " not sent (not above threshold "
                  << threshold << ")" << std::endl;
      }
    }
    const bool delivered = client.WaitForMeasurements(std::chrono::seconds(30));
    if (!delivered) {
      std::cerr << "Exiting with " << client.PendingMeasurements()
                << " undelivered measurements; the offline buffer is "
                   "memory-only." << std::endl;
    }
    return queued && delivered ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Client failed: " << error.what() << std::endl;
    return 1;
  }
}
