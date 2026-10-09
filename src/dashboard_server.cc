#include "dashboard_server.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "measure_service.h"
#include "measurement_utils.h"

namespace {

constexpr char kHttpBindAddress[] = "127.0.0.1";

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::string headers;
  std::string body;
};

bool SendAll(int client_fd, const std::string& data) {
  size_t total_sent = 0;
  while (total_sent < data.size()) {
    int send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent = send(client_fd, data.data() + total_sent,
                              data.size() - total_sent, send_flags);
    if (sent <= 0) return false;
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
  response << "X-Content-Type-Options: nosniff\r\n";
  response << "X-Frame-Options: DENY\r\n";
  response << "Connection: close\r\n\r\n";
  response << body;
  SendAll(client_fd, response.str());
}

std::string GetHeaderValue(const std::string& headers,
                           const std::string& header_name) {
  std::istringstream lines(headers);
  std::string line;
  std::getline(lines, line);
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (name != header_name) continue;
    size_t value_start = colon + 1;
    while (value_start < line.size() &&
           std::isspace(static_cast<unsigned char>(line[value_start]))) {
      ++value_start;
    }
    return line.substr(value_start);
  }
  return "";
}

bool ReadHttpRequest(int client_fd, HttpRequest* request) {
  constexpr size_t kMaxHeaderBytes = 16 * 1024;
  constexpr size_t kMaxBodyBytes = 4 * 1024;
  char buffer[1024];
  std::string request_text;
  size_t header_end = std::string::npos;
  while ((header_end = request_text.find("\r\n\r\n")) == std::string::npos) {
    const ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) return false;
    request_text.append(buffer, static_cast<size_t>(received));
    if (request_text.size() > kMaxHeaderBytes &&
        request_text.find("\r\n\r\n") == std::string::npos) {
      return false;
    }
  }
  if (header_end > kMaxHeaderBytes) return false;

  request->headers = request_text.substr(0, header_end);
  if (!GetHeaderValue(request->headers, "transfer-encoding").empty()) {
    return false;
  }
  size_t body_length = 0;
  const std::string content_length =
      GetHeaderValue(request->headers, "content-length");
  if (!content_length.empty()) {
    const auto parsed =
        std::from_chars(content_length.data(),
                        content_length.data() + content_length.size(),
                        body_length);
    if (parsed.ec != std::errc() ||
        parsed.ptr != content_length.data() + content_length.size() ||
        body_length > kMaxBodyBytes) {
      return false;
    }
  }

  const size_t request_length = header_end + 4 + body_length;
  while (request_text.size() < request_length) {
    const ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) return false;
    request_text.append(buffer, static_cast<size_t>(received));
  }
  request->body = request_text.substr(header_end + 4, body_length);
  std::istringstream request_line(request_text.substr(0, header_end));
  request_line >> request->method >> request->path >> request->version;
  return !request->method.empty() && !request->path.empty() &&
         !request->version.empty();
}

bool DecodeFormComponent(const std::string& input, std::string* output) {
  output->clear();
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      output->push_back(' ');
    } else if (input[i] == '%') {
      if (i + 2 >= input.size()) return false;
      unsigned int value = 0;
      const auto parsed = std::from_chars(input.data() + i + 1,
                                          input.data() + i + 3, value, 16);
      if (parsed.ec != std::errc() || parsed.ptr != input.data() + i + 3) {
        return false;
      }
      output->push_back(static_cast<char>(value));
      i += 2;
    } else {
      output->push_back(input[i]);
    }
  }
  return true;
}

bool ParseForm(const std::string& body,
               std::map<std::string, std::string>* form) {
  form->clear();
  size_t start = 0;
  while (start < body.size()) {
    const size_t end = body.find('&', start);
    const size_t pair_end = end == std::string::npos ? body.size() : end;
    const size_t equals = body.find('=', start);
    if (equals == std::string::npos || equals >= pair_end) return false;
    std::string key;
    std::string value;
    if (!DecodeFormComponent(body.substr(start, equals - start), &key) ||
        !DecodeFormComponent(body.substr(equals + 1, pair_end - equals - 1),
                             &value) ||
        !form->emplace(std::move(key), std::move(value)).second) {
      return false;
    }
    start = pair_end + 1;
  }
  return true;
}

bool ParseInteger(const std::map<std::string, std::string>& form,
                  const std::string& key, int* value) {
  const auto it = form.find(key);
  if (it == form.end() || it->second.empty()) return false;
  const auto parsed = std::from_chars(it->second.data(),
                                      it->second.data() + it->second.size(),
                                      *value);
  return parsed.ec == std::errc() &&
         parsed.ptr == it->second.data() + it->second.size();
}

bool ParseInteger64(const std::map<std::string, std::string>& form,
                    const std::string& key, int64_t* value) {
  const auto it = form.find(key);
  if (it == form.end() || it->second.empty()) return false;
  const auto parsed = std::from_chars(it->second.data(),
                                      it->second.data() + it->second.size(),
                                      *value);
  return parsed.ec == std::errc() &&
         parsed.ptr == it->second.data() + it->second.size();
}

bool ParseQuery(const std::string& path,
               std::map<std::string, std::string>* query) {
  const size_t query_start = path.find('?');
  if (query_start == std::string::npos) {
    query->clear();
    return true;
  }
  return ParseForm(path.substr(query_start + 1), query);
}

bool HasAllowedOrigin(const std::string& headers, uint16_t port) {
  const std::string origin = GetHeaderValue(headers, "origin");
  const std::string port_text = ":" + std::to_string(port);
  return origin == "http://127.0.0.1" + port_text ||
         origin == "http://localhost" + port_text;
}

void SendApiError(int client_fd, const std::string& status,
                  const std::string& message) {
  SendHttpResponse(client_fd, status, "application/json; charset=utf-8",
                   "{\"error\":\"" + measure::JsonEscape(message) + "\"}");
}

std::string BuildDashboardJson(const MeasureServiceImpl& service) {
  const auto measurements = service.GetMeasurementsSnapshot();
  const auto latest_measurements = service.GetLatestMeasurementsSnapshot();
  const auto connected = service.GetConnectedClients();
  std::set<std::string> client_ids(connected.begin(), connected.end());
  std::map<std::string, measure::StoredMeasurement> latest;
  for (const auto& measurement : latest_measurements) {
    client_ids.insert(measurement.client_id);
    latest[measurement.client_id] = measurement;
  }

  std::string json =
      "{\"threshold\":" + std::to_string(service.GetThreshold()) +
      ",\"devices\":[";
  bool first = true;
  for (const auto& client_id : client_ids) {
    if (!first) json += ",";
    first = false;
    const auto measurement = latest.find(client_id);
    const bool has_measurement = measurement != latest.end();
    const bool is_connected =
        std::find(connected.begin(), connected.end(), client_id) !=
        connected.end();
    json += "{\"client_id\":\"" + measure::JsonEscape(client_id) +
            "\",\"connected\":" + (is_connected ? "true" : "false") +
            ",\"latest_point\":";
    json += has_measurement ? std::to_string(measurement->second.point) : "null";
    json += ",\"timestamp_unix_ms\":" +
            std::to_string(has_measurement
                               ? measurement->second.timestamp_unix_ms
                               : 0) +
            "}";
  }
  json += "],\"events\":[";
  for (size_t i = 0; i < measurements.size(); ++i) {
    if (i > 0) json += ",";
    const auto& measurement = measurements[i];
    json += "{\"client_id\":\"" + measure::JsonEscape(measurement.client_id) +
            "\",\"point\":" + std::to_string(measurement.point) +
            ",\"timestamp_unix_ms\":" +
            std::to_string(measurement.timestamp_unix_ms) + "}";
  }
  json += "]}";
  return json;
}

std::string BuildEventsJson(const std::vector<measure::StoredEvent>& events) {
  std::string json = "{\"events\":[";
  for (size_t i = 0; i < events.size(); ++i) {
    if (i > 0) json += ",";
    const auto& event = events[i];
    json += "{\"id\":" + std::to_string(event.id) +
            ",\"event_type\":\"" + measure::JsonEscape(event.event_type) +
            "\",\"timestamp_unix_ms\":" +
            std::to_string(event.timestamp_unix_ms) + ",\"client_id\":\"" +
            measure::JsonEscape(event.client_id) + "\",\"point\":";
    json += event.point.has_value() ? std::to_string(*event.point) : "null";
    json += ",\"threshold\":";
    json += event.threshold.has_value() ? std::to_string(*event.threshold) : "null";
    json += ",\"mode\":\"" + measure::JsonEscape(event.mode) +
            "\",\"payload\":" + event.payload_json + "}";
  }
  json += "]}";
  return json;
}

std::string BuildTrendsJson(
    const std::vector<measure::MeasurementTrend>& trends) {
  std::string json = "{\"trends\":[";
  for (size_t i = 0; i < trends.size(); ++i) {
    if (i > 0) json += ",";
    const auto& trend = trends[i];
    json += "{\"client_id\":\"" + measure::JsonEscape(trend.client_id) +
            "\",\"bucket_start_unix_ms\":" +
            std::to_string(trend.bucket_start_unix_ms) +
            ",\"count\":" + std::to_string(trend.count) +
            ",\"average\":" + std::to_string(trend.average) +
            ",\"minimum\":" + std::to_string(trend.minimum) +
            ",\"maximum\":" + std::to_string(trend.maximum) + "}";
  }
  json += "]}";
  return json;
}

bool ParseHistoryQuery(const std::string& request_path,
                       std::map<std::string, std::string>* query,
                       int64_t* since, int64_t* until, std::string* client,
                       size_t* limit, size_t default_limit,
                       size_t maximum_limit) {
  if (!ParseQuery(request_path, query)) return false;
  *since = 0;
  *until = 0;
  *client = "";
  *limit = default_limit;
  if (query->count("since_ms") && !ParseInteger64(*query, "since_ms", since)) {
    return false;
  }
  if (query->count("until_ms") && !ParseInteger64(*query, "until_ms", until)) {
    return false;
  }
  if (*since < 0 || *until < 0 ||
      (*since > 0 && *until > 0 && *until < *since)) {
    return false;
  }
  if (const auto it = query->find("client_id"); it != query->end()) {
    *client = it->second;
  }
  if (query->count("limit")) {
    int64_t parsed_limit = 0;
    if (!ParseInteger64(*query, "limit", &parsed_limit) || parsed_limit < 1 ||
        static_cast<uint64_t>(parsed_limit) > maximum_limit) {
      return false;
    }
    *limit = static_cast<size_t>(parsed_limit);
  }
  return true;
}

std::string CsvField(const std::string& field) {
  std::string value = field;
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first != std::string::npos &&
      (value[first] == '=' || value[first] == '+' || value[first] == '-' ||
       value[first] == '@')) {
    value.insert(0, "'");
  }
  std::string escaped = "\"";
  for (char character : value) {
    if (character == '"') escaped += '"';
    escaped += character;
  }
  escaped += '"';
  return escaped;
}

std::string BuildEventsCsv(const std::vector<measure::StoredEvent>& events) {
  std::string csv =
      "id,event_type,timestamp_unix_ms,client_id,point,threshold,mode,payload\n";
  for (const auto& event : events) {
    csv += std::to_string(event.id) + "," + CsvField(event.event_type) + "," +
           std::to_string(event.timestamp_unix_ms) + "," +
           CsvField(event.client_id) + ",";
    csv += event.point.has_value() ? std::to_string(*event.point) : "";
    csv += ",";
    csv += event.threshold.has_value() ? std::to_string(*event.threshold) : "";
    csv += "," + CsvField(event.mode) + "," + CsvField(event.payload_json) +
           "\r\n";
  }
  return csv;
}

void HandlePost(int client_fd, const HttpRequest& request,
                MeasureServiceImpl& service, uint16_t port) {
  if (!HasAllowedOrigin(request.headers, port)) {
    SendApiError(client_fd, "403 Forbidden",
                 "A local dashboard origin is required.");
    return;
  }
  if (GetHeaderValue(request.headers, "content-length").empty()) {
    SendApiError(client_fd, "400 Bad Request", "Content-Length is required.");
    return;
  }

  std::map<std::string, std::string> form;
  if (!ParseForm(request.body, &form)) {
    SendApiError(client_fd, "400 Bad Request", "Invalid form data.");
    return;
  }
  if (request.path == "/api/threshold") {
    int threshold = 0;
    if (!ParseInteger(form, "threshold", &threshold) || threshold < 0 ||
        threshold > 100000) {
      SendApiError(client_fd, "400 Bad Request",
                   "Threshold must be an integer from 0 to 100000.");
      return;
    }
    service.SetThreshold(threshold);
    SendHttpResponse(client_fd, "200 OK", "application/json; charset=utf-8",
                     "{\"message\":\"Fleet threshold updated.\"}");
    return;
  }
  if (request.path == "/api/calibration") {
    int duration = 0;
    if (!ParseInteger(form, "duration_seconds", &duration) || duration < 1 ||
        duration > 3600) {
      SendApiError(client_fd, "400 Bad Request",
                   "Calibration duration must be from 1 to 3600 seconds.");
      return;
    }
    const auto target = form.find("client_id");
    const std::string client_id =
        target == form.end() ? "" : target->second;
    if (!client_id.empty()) {
      const auto clients = service.GetConnectedClients();
      if (std::find(clients.begin(), clients.end(), client_id) ==
          clients.end()) {
        SendApiError(client_fd, "404 Not Found",
                     "The selected device is not connected.");
        return;
      }
    }
    service.StartCalibration(client_id, duration);
    SendHttpResponse(client_fd, "200 OK", "application/json; charset=utf-8",
                     "{\"message\":\"Calibration started.\"}");
    return;
  }
  SendApiError(client_fd, "404 Not Found", "Not found.");
}

std::string ContentType(const std::filesystem::path& path) {
  const std::string extension = path.extension().string();
  if (extension == ".html") return "text/html; charset=utf-8";
  if (extension == ".js" || extension == ".mjs") {
    return "text/javascript; charset=utf-8";
  }
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".png") return "image/png";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".json") return "application/json; charset=utf-8";
  if (extension == ".woff2") return "font/woff2";
  return "application/octet-stream";
}

bool IsInsideRoot(const std::filesystem::path& root,
                  const std::filesystem::path& file) {
  const auto relative = file.lexically_relative(root);
  return !relative.empty() && *relative.begin() != ".." && !relative.is_absolute();
}

void ServeStaticFile(int client_fd, const std::string& request_path,
                     const std::string& web_root) {
  if (request_path.find('%') != std::string::npos ||
      request_path.find('\\') != std::string::npos) {
    SendHttpResponse(client_fd, "404 Not Found", "text/plain; charset=utf-8",
                     "Not found.\n");
    return;
  }
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(web_root);
  std::string path = request_path.substr(0, request_path.find('?'));
  std::filesystem::path relative(path == "/" ? "index.html" : path.substr(1));
  for (const auto& component : relative) {
    if (component == ".." || component == ".") {
      SendHttpResponse(client_fd, "404 Not Found",
                       "text/plain; charset=utf-8", "Not found.\n");
      return;
    }
  }

  std::error_code error;
  auto file = std::filesystem::weakly_canonical(root / relative, error);
  if (error || !IsInsideRoot(root, file) || !std::filesystem::is_regular_file(file)) {
    if (relative.has_extension()) {
      SendHttpResponse(client_fd, "404 Not Found",
                       "text/plain; charset=utf-8", "Not found.\n");
      return;
    }
    error.clear();
    file = std::filesystem::weakly_canonical(root / "index.html", error);
    if (error || !IsInsideRoot(root, file) ||
        !std::filesystem::is_regular_file(file)) {
      SendHttpResponse(client_fd, "404 Not Found",
                       "text/plain; charset=utf-8",
                       "Dashboard build not found. Run npm run build in dashboard/.\n");
      return;
    }
  }

  std::ifstream input(file, std::ios::binary);
  if (!input) {
    SendHttpResponse(client_fd, "404 Not Found", "text/plain; charset=utf-8",
                     "Dashboard build not found. Run npm run build in dashboard/.\n");
    return;
  }
  std::ostringstream body;
  body << input.rdbuf();
  SendHttpResponse(client_fd, "200 OK", ContentType(file), body.str());
}

void HandleHttpClient(int client_fd, MeasureServiceImpl& service,
                      uint16_t port, const std::string& web_root) {
  HttpRequest request;
  if (!ReadHttpRequest(client_fd, &request)) {
    SendHttpResponse(client_fd, "400 Bad Request", "text/plain; charset=utf-8",
                     "Invalid request.\n");
    return;
  }
  if (request.method == "POST") {
    try {
      HandlePost(client_fd, request, service, port);
    } catch (const std::exception& error) {
      SendApiError(client_fd, "500 Internal Server Error", error.what());
    }
    return;
  }
  if (request.method != "GET") {
    SendHttpResponse(client_fd, "405 Method Not Allowed",
                     "text/plain; charset=utf-8", "Method not allowed.\n");
    return;
  }

  const std::string path = request.path.substr(0, request.path.find('?'));
  try {
    if (path == "/api/dashboard") {
      SendHttpResponse(client_fd, "200 OK", "application/json; charset=utf-8",
                       BuildDashboardJson(service));
    } else if (path == "/api/history") {
      std::map<std::string, std::string> query;
      int64_t since = 0;
      int64_t until = 0;
      std::string client_id;
      size_t limit = 0;
      if (!ParseHistoryQuery(request.path, &query, &since, &until, &client_id,
                             &limit, 1000, 10000)) {
        SendApiError(client_fd, "400 Bad Request",
                     "Invalid history query; use valid since_ms, until_ms, "
                     "client_id, and limit values.");
      } else {
        SendHttpResponse(
            client_fd, "200 OK", "application/json; charset=utf-8",
            BuildEventsJson(
                service.GetEventsSnapshot(limit, since, until, client_id)));
      }
    } else if (path == "/api/trends") {
      std::map<std::string, std::string> query;
      int64_t since = 0;
      int64_t until = 0;
      std::string client_id;
      size_t ignored_limit = 0;
      int64_t bucket_ms = 3600000;
      if (!ParseHistoryQuery(request.path, &query, &since, &until, &client_id,
                             &ignored_limit, 10000, 10000) ||
          (query.count("bucket_ms") &&
           (!ParseInteger64(query, "bucket_ms", &bucket_ms) ||
            bucket_ms < 1 || bucket_ms > 2678400000LL))) {
        SendApiError(client_fd, "400 Bad Request",
                     "Invalid trend query or bucket_ms value.");
      } else {
        SendHttpResponse(
            client_fd, "200 OK", "application/json; charset=utf-8",
            BuildTrendsJson(service.GetTrends(bucket_ms, since, until,
                                              client_id)));
      }
    } else if (path == "/api/export") {
      std::map<std::string, std::string> query;
      int64_t since = 0;
      int64_t until = 0;
      std::string client_id;
      size_t limit = static_cast<size_t>(
          std::numeric_limits<int64_t>::max());
      if (!ParseHistoryQuery(
              request.path, &query, &since, &until, &client_id, &limit,
              limit, limit)) {
        SendApiError(client_fd, "400 Bad Request", "Invalid export query.");
      } else if (query["format"] == "csv") {
        SendHttpResponse(
            client_fd, "200 OK", "text/csv; charset=utf-8",
            BuildEventsCsv(
                service.GetEventsSnapshot(limit, since, until, client_id)));
      } else if (query["format"].empty() || query["format"] == "json") {
        SendHttpResponse(
            client_fd, "200 OK", "application/json; charset=utf-8",
            BuildEventsJson(
                service.GetEventsSnapshot(limit, since, until, client_id)));
      } else {
        SendApiError(client_fd, "400 Bad Request",
                     "Export format must be json or csv.");
      }
    } else if (path == "/measurements.json") {
      SendHttpResponse(client_fd, "200 OK", "application/json; charset=utf-8",
                       measure::BuildMeasurementsJson(
                           service.GetMeasurementsSnapshot()));
    } else if (path.rfind("/api/", 0) == 0) {
      SendApiError(client_fd, "404 Not Found", "Not found.");
    } else {
      ServeStaticFile(client_fd, request.path, web_root);
    }
  } catch (const std::exception& error) {
    SendApiError(client_fd, "500 Internal Server Error", error.what());
  }
}

}  // namespace

DashboardServer::DashboardServer(uint16_t port, MeasureServiceImpl& service,
                                 std::string web_root)
    : port_(port), service_(service), web_root_(std::move(web_root)) {}

DashboardServer::~DashboardServer() { Stop(); }

void DashboardServer::Start() {
  if (!thread_.joinable()) thread_ = std::thread(&DashboardServer::Run, this);
}

void DashboardServer::Stop() {
  stop_requested_.store(true);
  CloseServerSocket();
  if (thread_.joinable()) thread_.join();
}

void DashboardServer::CloseServerSocket() {
  const int server_fd = server_fd_.exchange(-1);
  if (server_fd >= 0) {
    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
  }
}

void DashboardServer::Run() {
  const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
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
    std::cerr << "Failed to bind dashboard server on " << kHttpBindAddress
              << ":" << port_ << ": " << std::strerror(errno) << std::endl;
    CloseServerSocket();
    return;
  }
  if (listen(server_fd, 16) < 0) {
    std::cerr << "Failed to listen on dashboard server: "
              << std::strerror(errno) << std::endl;
    CloseServerSocket();
    return;
  }

  std::cout << "Fleet dashboard available at http://" << kHttpBindAddress << ":"
            << port_ << std::endl;
  while (!stop_requested_.load()) {
    const int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (stop_requested_.load()) break;
      std::cerr << "HTTP accept failed: " << std::strerror(errno) << std::endl;
      continue;
    }
    HandleHttpClient(client_fd, service_, port_, web_root_);
    close(client_fd);
  }
  CloseServerSocket();
}
