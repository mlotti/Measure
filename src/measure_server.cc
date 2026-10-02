#include <iostream>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <utility>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "measure_service.h"
#include "measurement_utils.h"

#ifdef BAZEL_BUILD
#include "measure.grpc.pb.h"
#else
#include "measure.grpc.pb.h"
#endif

#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::Status;
using measure::CalibrationRequest;
using measure::CalibrationResponse;
using measure::ClientInfo;
using measure::Command;
using measure::Measure;
using measure::Measurement;
using measure::Mode;
using measure::Thumbs;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");
ABSL_FLAG(uint16_t, http_port, 8080, "HTTP port for the localhost graph UI");
ABSL_FLAG(int, samples_retained, 500,
          "Maximum number of recent measurements retained for the graph");

// Default threshold: only measurements above this value are stored.
constexpr int kDefaultThreshold = 8;
constexpr char kHttpBindAddress[] = "127.0.0.1";

std::string BuildGraphHtml() {
  return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Measure graph</title>
  <style>
    body { font-family: sans-serif; margin: 24px; background: #f7f7f7; color: #222; }
    h1 { margin-bottom: 8px; }
    #meta { margin-bottom: 16px; color: #555; }
    #legend { display: flex; flex-wrap: wrap; gap: 12px; margin: 12px 0 20px; }
    .legend-item { display: flex; align-items: center; gap: 8px; background: #fff; padding: 6px 10px; border-radius: 999px; }
    .swatch { width: 12px; height: 12px; border-radius: 999px; }
    canvas { background: #fff; border-radius: 12px; box-shadow: 0 1px 4px rgba(0,0,0,0.08); max-width: 100%; }
  </style>
</head>
<body>
  <h1>Measurements</h1>
  <div id="meta">Loading…</div>
  <canvas id="chart" width="960" height="480"></canvas>
  <div id="legend"></div>
  <script>
    const canvas = document.getElementById('chart');
    const ctx = canvas.getContext('2d');
    const legend = document.getElementById('legend');
    const meta = document.getElementById('meta');
    const palette = ['#1f77b4', '#d62728', '#2ca02c', '#9467bd', '#ff7f0e', '#8c564b', '#17becf', '#e377c2'];

    function colourForClient(clientId, indexMap) {
      if (!(clientId in indexMap)) {
        indexMap[clientId] = Object.keys(indexMap).length;
      }
      return palette[indexMap[clientId] % palette.length];
    }

    function draw(data) {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      const margin = { top: 20, right: 20, bottom: 40, left: 50 };
      const width = canvas.width - margin.left - margin.right;
      const height = canvas.height - margin.top - margin.bottom;
      const points = data.measurements || [];

      ctx.strokeStyle = '#d0d0d0';
      ctx.lineWidth = 1;
      ctx.strokeRect(margin.left, margin.top, width, height);

      if (!points.length) {
        ctx.fillStyle = '#666';
        ctx.fillText('No measurements yet', margin.left + 20, margin.top + 30);
        legend.innerHTML = '';
        meta.textContent = `Showing 0 measurements`;
        return;
      }

      const times = points.map((p) => p.timestamp_unix_ms);
      const values = points.map((p) => p.point);
      const minTime = Math.min(...times);
      const maxTime = Math.max(...times);
      const minValue = Math.min(...values);
      const maxValue = Math.max(...values);
      const timeSpan = Math.max(maxTime - minTime, 1);
      const valueSpan = Math.max(maxValue - minValue, 1);
      const clientOrder = {};
      const byClient = {};

      for (const point of points) {
        byClient[point.client_id] = byClient[point.client_id] || [];
        byClient[point.client_id].push(point);
      }

      Object.values(byClient).forEach(series =>
        series.sort((a, b) => a.timestamp_unix_ms - b.timestamp_unix_ms)
      );

      ctx.font = '12px sans-serif';
      ctx.fillStyle = '#444';
      ctx.fillText(new Date(minTime).toLocaleTimeString(), margin.left, canvas.height - 12);
      ctx.fillText(new Date(maxTime).toLocaleTimeString(), canvas.width - margin.right - 70, canvas.height - 12);
      ctx.fillText(String(maxValue), 12, margin.top + 4);
      ctx.fillText(String(minValue), 12, canvas.height - margin.bottom + 4);

      legend.innerHTML = '';
      for (const [clientId, series] of Object.entries(byClient)) {
        const colour = colourForClient(clientId, clientOrder);
        const legendItem = document.createElement('div');
        legendItem.className = 'legend-item';
        const swatch = document.createElement('span');
        swatch.className = 'swatch';
        swatch.style.background = colour;
        const label = document.createElement('span');
        label.textContent = clientId;
        legendItem.appendChild(swatch);
        legendItem.appendChild(label);
        legend.appendChild(legendItem);

        ctx.strokeStyle = colour;
        ctx.fillStyle = colour;
        ctx.lineWidth = 2;
        ctx.beginPath();
        series.forEach((point, index) => {
          const x = margin.left + ((point.timestamp_unix_ms - minTime) / timeSpan) * width;
          const y = margin.top + height - ((point.point - minValue) / valueSpan) * height;
          if (index === 0) {
            ctx.moveTo(x, y);
          } else {
            ctx.lineTo(x, y);
          }
        });
        ctx.stroke();

        series.forEach((point) => {
          const x = margin.left + ((point.timestamp_unix_ms - minTime) / timeSpan) * width;
          const y = margin.top + height - ((point.point - minValue) / valueSpan) * height;
          ctx.beginPath();
          ctx.arc(x, y, 4, 0, Math.PI * 2);
          ctx.fill();
        });
      }

      meta.textContent = `Showing ${points.length} measurements across ${Object.keys(byClient).length} clients · refreshes every 2s`;
    }

    async function refresh() {
      try {
        const response = await fetch('/measurements.json', { cache: 'no-store' });
        const data = await response.json();
        draw(data);
      } catch (error) {
        meta.textContent = `Failed to load measurements: ${error}`;
      }
    }

    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>)HTML";
}

bool SendAll(int client_fd, const std::string& data) {
  size_t total_sent = 0;
  while (total_sent < data.size()) {
    int send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags |= MSG_NOSIGNAL;
#endif
    ssize_t sent = send(client_fd, data.data() + total_sent,
                        data.size() - total_sent, send_flags);
    if (sent <= 0) {
      return false;
    }
    total_sent += static_cast<size_t>(sent);
  }
  return true;
}

void SendHttpResponse(int client_fd, const std::string& status,
                      const std::string& content_type,
                      const std::string& body) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status << "\r\n";
  response << "Content-Type: " << content_type << "\r\n";
  response << "Content-Length: " << body.size() << "\r\n";
  response << "Cache-Control: no-store\r\n";
  response << "Connection: close\r\n\r\n";
  response << body;
  const std::string response_text = response.str();
  SendAll(client_fd, response_text);
}

bool ReadHttpRequest(int client_fd, std::string* request_text) {
  constexpr size_t kMaxHeaderBytes = 16 * 1024;
  char buffer[1024];
  request_text->clear();

  while (request_text->find("\r\n\r\n") == std::string::npos) {
    ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      return false;
    }
    request_text->append(buffer, static_cast<size_t>(received));
    if (request_text->size() > kMaxHeaderBytes) {
      return false;
    }
  }
  return true;
}

void HandleHttpClient(int client_fd, const MeasureServiceImpl& service) {
  std::string request_text;
  if (!ReadHttpRequest(client_fd, &request_text)) {
    SendHttpResponse(client_fd, "400 Bad Request", "text/plain; charset=utf-8",
                     "Invalid request.\n");
    return;
  }

  std::istringstream request(request_text);
  std::string method;
  std::string path;
  std::string version;
  request >> method >> path >> version;

  if (method.empty() || path.empty() || version.empty()) {
    SendHttpResponse(client_fd, "400 Bad Request", "text/plain; charset=utf-8",
                     "Malformed request line.\n");
    return;
  }

  if (method != "GET") {
    SendHttpResponse(client_fd, "405 Method Not Allowed", "text/plain; charset=utf-8",
                     "Only GET is supported.\n");
    return;
  }

  if (path == "/" || path == "/index.html") {
    SendHttpResponse(client_fd, "200 OK", "text/html; charset=utf-8",
                     BuildGraphHtml());
    return;
  }

  if (path == "/measurements.json") {
    SendHttpResponse(client_fd, "200 OK", "application/json; charset=utf-8",
                     measure::BuildMeasurementsJson(
                         service.GetMeasurementsSnapshot()));
    return;
  }

  SendHttpResponse(client_fd, "404 Not Found", "text/plain; charset=utf-8",
                   "Not found.\n");
}

class HttpServer {
 public:
  HttpServer(uint16_t port, const MeasureServiceImpl& service)
      : port_(port), service_(service) {}

  void Start() { thread_ = std::thread(&HttpServer::Run, this); }

  void Stop() {
    stop_requested_.store(true);
    int server_fd = server_fd_.exchange(-1);
    if (server_fd >= 0) {
      shutdown(server_fd, SHUT_RDWR);
      close(server_fd);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void CloseServerSocket() {
    int server_fd = server_fd_.exchange(-1);
    if (server_fd >= 0) {
      shutdown(server_fd, SHUT_RDWR);
      close(server_fd);
    }
  }

  void Run() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
      std::cerr << "Failed to create HTTP socket" << std::endl;
      return;
    }
    server_fd_.store(server_fd);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port_);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
        0) {
      std::cerr << "Failed to bind HTTP server on " << kHttpBindAddress << ":"
                << port_ << ": " << std::strerror(errno) << std::endl;
      CloseServerSocket();
      return;
    }

    if (listen(server_fd, 16) < 0) {
      std::cerr << "Failed to listen on HTTP server: " << std::strerror(errno)
                << std::endl;
      CloseServerSocket();
      return;
    }

    std::cout << "Graph UI available at http://" << kHttpBindAddress << ":"
              << port_ << std::endl;

    while (!stop_requested_.load()) {
      int client_fd = accept(server_fd, nullptr, nullptr);
      if (client_fd < 0) {
        if (stop_requested_.load()) {
          break;
        }
        std::cerr << "HTTP accept failed: " << std::strerror(errno)
                  << std::endl;
        continue;
      }
      HandleHttpClient(client_fd, service_);
      close(client_fd);
    }

    CloseServerSocket();
  }

  uint16_t port_;
  const MeasureServiceImpl& service_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> server_fd_{-1};
  std::thread thread_;
};

void RunServer(uint16_t port, uint16_t http_port, size_t max_measurements) {
  std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
  MeasureServiceImpl service(max_measurements);
  HttpServer http_server(http_port, service);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  http_server.Start();

  server->Wait();
  http_server.Stop();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  RunServer(absl::GetFlag(FLAGS_port), absl::GetFlag(FLAGS_http_port),
            static_cast<size_t>(
                std::max(1, absl::GetFlag(FLAGS_samples_retained))));
  return 0;
}
