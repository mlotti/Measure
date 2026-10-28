#include <chrono>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include <poll.h>
#include <unistd.h>

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

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void RequestStop(int) { stop_requested = 1; }

bool WaitForInput(bool* input_open) {
  pollfd input{STDIN_FILENO, POLLIN, 0};
  const int result = poll(*input_open ? &input : nullptr,
                          *input_open ? 1 : 0, 100);
  if (result < 0) {
    if (errno == EINTR) return false;
    throw std::runtime_error("Unable to poll terminal input.");
  }
  if (result == 0) return false;
  if (input.revents & (POLLERR | POLLNVAL)) {
    throw std::runtime_error("Terminal input is unavailable.");
  }
  if (input.revents & (POLLIN | POLLHUP)) {
    char buffer[256];
    const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) return false;
      throw std::runtime_error("Unable to read terminal input.");
    }
    if (count == 0) *input_open = false;
    for (ssize_t i = 0; i < count; ++i) {
      if (buffer[i] == 'q') return true;
    }
  }
  return false;
}

}

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
    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);
    client.Start();
    std::cout << "Measuring every 3 seconds. Press Ctrl+C or enter q to quit."
              << std::endl;
    std::cout << "Waiting for initial command before sampling." << std::endl;

    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(0, 10);
    bool queued = true;
    bool input_open = true;
    auto next_sample = std::chrono::steady_clock::now();
    while (!stop_requested && !client.HasFailed()) {
      if (WaitForInput(&input_open) || stop_requested) break;
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_sample &&
          client.WaitForCommand(std::chrono::milliseconds(0))) {
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
        next_sample = now + std::chrono::seconds(3);
      }
    }
    std::cout << "Sampling stopped; draining buffered measurements (up to 30 seconds)."
              << std::endl;
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
