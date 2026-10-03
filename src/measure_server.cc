#include <iostream>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <utility>
#include <vector>

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

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using grpc::Server;
using grpc::ServerBuilder;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");
ABSL_FLAG(uint16_t, http_port, 8080, "HTTP port for the localhost dashboard");
ABSL_FLAG(int, samples_retained, 500,
          "Maximum number of recent measurements retained for the dashboard");

constexpr char kHttpBindAddress[] = "127.0.0.1";

std::string BuildDashboardJson(const MeasureServiceImpl& service) {
  const auto measurements = service.GetMeasurementsSnapshot();
  const auto connected = service.GetConnectedClients();
  std::set<std::string> client_ids(connected.begin(), connected.end());
  std::map<std::string, measure::StoredMeasurement> latest;
  for (const auto& measurement : measurements) {
    client_ids.insert(measurement.client_id);
    latest[measurement.client_id] = measurement;
  }

  std::string json = "{\"threshold\":" + std::to_string(service.GetThreshold()) +
                     ",\"devices\":[";
  bool first = true;
  for (const auto& client_id : client_ids) {
    if (!first) json += ",";
    first = false;
    const auto measurement = latest.find(client_id);
    const bool has_measurement = measurement != latest.end();
    const bool is_connected =
        std::find(connected.begin(), connected.end(), client_id) != connected.end();
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

std::string BuildDashboardHtml() {
  return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Measure | Fleet dashboard</title>
  <style>
    * { box-sizing: border-box; }
    body { margin: 0; color: #292d32; background: #f1f3f5; font: 15px Arial, sans-serif; }
    header { height: 72px; display: flex; align-items: center; gap: 24px; padding: 0 24px; color: #fff; background: #2196ed; box-shadow: 0 2px 5px #0003; }
    header strong { font-size: 23px; letter-spacing: .2px; }
    header nav { display: flex; align-items: center; gap: 22px; font-weight: 600; }
    header nav span { white-space: nowrap; }
    .online { margin-left: auto; padding: 9px 14px; border-radius: 4px; background: #1685d9; }
    main { display: grid; grid-template-columns: minmax(285px, 1fr) minmax(0, 2.65fr); gap: 12px; padding: 14px; align-items: start; }
    .column { display: grid; gap: 12px; min-width: 0; }
    .widget { min-width: 0; background: #fff; border: 1px solid #d5d8da; border-radius: 5px; box-shadow: 0 1px 3px #0002; }
    .widget[draggable="true"] { cursor: grab; }
    .widget.dragging { opacity: .55; }
    .title { display: flex; justify-content: space-between; align-items: center; padding: 14px 16px; border-bottom: 1px solid #eee; font-size: 19px; }
    .tools { color: #92979b; font-size: 17px; letter-spacing: 8px; }
    .content { padding: 14px 16px; }
    .summary { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
    .metric { padding: 14px; background: #f7f8f9; border-left: 3px solid #2196ed; }
    .metric strong { display: block; font-size: 25px; margin-bottom: 4px; }
    .muted { color: #70767b; }
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 10px 9px; text-align: left; border-bottom: 1px solid #dedede; }
    th { color: #73777b; font-weight: 600; }
    tbody tr:nth-child(even) { background: #f6f6f6; }
    .status { display: inline-flex; align-items: center; gap: 7px; }
    .dot { width: 9px; height: 9px; border-radius: 50%; background: #999; }
    .dot.online-dot { background: #27a66b; }
    .controls { display: flex; flex-wrap: wrap; align-items: end; gap: 10px; }
    label { display: grid; gap: 5px; color: #676c70; font-size: 13px; }
    input, select { min-height: 36px; padding: 7px 9px; border: 1px solid #c8cccf; border-radius: 3px; background: #fff; color: #292d32; font: inherit; }
    button { min-height: 36px; padding: 8px 14px; border: 0; border-radius: 3px; color: white; background: #2196ed; font: 600 14px Arial, sans-serif; cursor: pointer; }
    button:hover { background: #147fc9; }
    button.secondary { background: #69747c; }
    #notice { min-height: 20px; margin-top: 9px; color: #27764e; }
    #chart { width: 100%; height: 300px; display: block; }
    .history { max-height: 345px; overflow: auto; }
    .empty { padding: 16px 10px; color: #777; }
    @media (max-width: 850px) { main { grid-template-columns: 1fr; } header { gap: 14px; padding: 0 14px; } header nav { gap: 12px; } }
    @media (max-width: 560px) { header { height: auto; min-height: 64px; flex-wrap: wrap; padding: 12px; } header nav { order: 3; width: 100%; overflow: auto; } .online { margin-left: auto; } main { padding: 8px; } .summary { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <header>
    <strong>Measure</strong>
    <nav><span>▦ &nbsp;Fleet</span><span>›</span><span>▤ &nbsp;Devices</span><span>›</span><span>⌁ &nbsp;Activity</span></nav>
    <span class="online">● &nbsp;Server online</span>
  </header>
  <main id="dashboard">
    <div class="column">
      <section class="widget" draggable="true" data-widget="overview">
        <div class="title">Fleet overview <span class="tools">ⓘ ↻ ⌃</span></div>
        <div class="content"><div class="summary">
          <div class="metric"><strong id="device-count">—</strong><span class="muted">Devices</span></div>
          <div class="metric"><strong id="online-count">—</strong><span class="muted">Online</span></div>
          <div class="metric"><strong id="threshold-value">—</strong><span class="muted">Fleet threshold</span></div>
        </div></div>
      </section>
      <section class="widget" draggable="true" data-widget="threshold">
        <div class="title">Threshold configuration <span class="tools">ⓘ ↻ ⌃</span></div>
        <div class="content">
          <form id="threshold-form" class="controls">
            <label>Measurement threshold<input name="threshold" type="number" min="0" max="100000" required></label>
            <button type="submit">Apply to fleet</button>
          </form><div id="notice" role="status"></div>
        </div>
      </section>
      <section class="widget" draggable="true" data-widget="calibration">
        <div class="title">Calibration <span class="tools">ⓘ ↻ ⌃</span></div>
        <div class="content">
          <form id="calibration-form" class="controls">
            <label>Target<select name="client_id"><option value="">All connected devices</option></select></label>
            <label>Duration (seconds)<input name="duration_seconds" type="number" min="1" max="3600" value="10" required></label>
            <button type="submit" class="secondary">Start calibration</button>
          </form>
        </div>
      </section>
    </div>
    <div class="column">
      <section class="widget" draggable="true" data-widget="chart">
        <div class="title">Fleet measurement chart <span class="tools">ⓘ ↻</span></div>
        <div class="content"><canvas id="chart" aria-label="Recent measurements by device"></canvas></div>
      </section>
      <section class="widget" draggable="true" data-widget="devices">
        <div class="title">Devices <span class="tools">ⓘ ↻ ⌃</span></div>
        <div class="content" id="devices"></div>
      </section>
      <section class="widget" draggable="true" data-widget="history">
        <div class="title">Event history <span class="tools">ⓘ ↻ ⌃</span></div>
        <div class="history" id="history"></div>
      </section>
    </div>
  </main>
  <script>
    const canvas = document.getElementById('chart');
    const ctx = canvas.getContext('2d');
    const palette = ['#2196ed', '#dc554d', '#34a36a', '#945bb5', '#f39c36', '#5785a4'];
    let currentEvents = [];

    function draw(events) {
      const bounds = canvas.getBoundingClientRect();
      const scale = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, bounds.width * scale);
      canvas.height = Math.max(1, bounds.height * scale);
      ctx.setTransform(scale, 0, 0, scale, 0, 0);
      const width = bounds.width, height = bounds.height;
      ctx.clearRect(0, 0, width, height);
      const margin = { top: 18, right: 18, bottom: 30, left: 44 };
      const points = events.slice().sort((a, b) =>
        a.timestamp_unix_ms - b.timestamp_unix_ms).slice(-120);
      ctx.strokeStyle = '#d9dcde';
      ctx.beginPath();
      ctx.moveTo(margin.left, margin.top);
      ctx.lineTo(margin.left, height - margin.bottom);
      ctx.lineTo(width - margin.right, height - margin.bottom);
      ctx.stroke();
      if (!points.length) {
        ctx.fillStyle = '#777';
        ctx.fillText('No measurements recorded yet', margin.left + 12, margin.top + 22);
        return;
      }
      const values = points.map(point => point.point);
      const min = Math.min(...values), max = Math.max(...values);
      const span = Math.max(max - min, 1);
      const clients = [...new Set(points.map(point => point.client_id))];
      clients.forEach((client, index) => {
        const series = points.filter(point => point.client_id === client);
        ctx.strokeStyle = palette[index % palette.length];
        ctx.lineWidth = 2;
        ctx.beginPath();
        series.forEach((point, position) => {
          const x = margin.left + (point.timestamp_unix_ms - points[0].timestamp_unix_ms) /
            Math.max(points[points.length - 1].timestamp_unix_ms - points[0].timestamp_unix_ms, 1) *
            (width - margin.left - margin.right);
          const y = height - margin.bottom - (point.point - min) / span *
            (height - margin.top - margin.bottom);
          if (position === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        });
        ctx.stroke();
      });
      ctx.fillStyle = '#60666a';
      ctx.fillText(String(max), 8, margin.top + 4);
      ctx.fillText(String(min), 8, height - margin.bottom);
    }

    function renderDevices(devices) {
      const root = document.getElementById('devices');
      const select = document.querySelector('#calibration-form select');
      const previousTarget = select.value;
      root.replaceChildren();
      select.replaceChildren(new Option('All connected devices', ''));
      if (!devices.length) {
        root.textContent = 'No devices have connected or sent measurements yet.';
        return;
      }
      const table = document.createElement('table');
      table.innerHTML = '<thead><tr><th>Device</th><th>Status</th><th>Latest reading</th><th>Last seen</th></tr></thead>';
      const body = document.createElement('tbody');
      devices.forEach(device => {
        const row = document.createElement('tr');
        const name = document.createElement('td');
        name.textContent = device.client_id;
        const status = document.createElement('td');
        const badge = document.createElement('span');
        badge.className = 'status';
        const dot = document.createElement('span');
        dot.className = device.connected ? 'dot online-dot' : 'dot';
        badge.append(dot, document.createTextNode(device.connected ? 'Online' : 'Offline'));
        status.append(badge);
        const reading = document.createElement('td');
        reading.textContent = device.latest_point === null ? '—' : device.latest_point;
        const time = document.createElement('td');
        time.textContent = device.timestamp_unix_ms ? new Date(device.timestamp_unix_ms).toLocaleString() : '—';
        row.append(name, status, reading, time);
        body.append(row);
        if (device.connected) select.add(new Option(device.client_id, device.client_id));
      });
      table.append(body);
      root.append(table);
      if ([...select.options].some(option => option.value === previousTarget)) select.value = previousTarget;
    }

    function renderHistory(events) {
      const root = document.getElementById('history');
      root.replaceChildren();
      if (!events.length) {
        root.textContent = 'No measurement events yet.';
        root.className = 'history empty';
        return;
      }
      root.className = 'history';
      const table = document.createElement('table');
      table.innerHTML = '<thead><tr><th>Time</th><th>Device</th><th>Measurement</th></tr></thead>';
      const body = document.createElement('tbody');
      events.slice().reverse().slice(0, 100).forEach(event => {
        const row = document.createElement('tr');
        const time = document.createElement('td');
        time.textContent = new Date(event.timestamp_unix_ms).toLocaleString();
        const device = document.createElement('td');
        device.textContent = event.client_id;
        const point = document.createElement('td');
        point.textContent = event.point;
        row.append(time, device, point);
        body.append(row);
      });
      table.append(body);
      root.append(table);
    }

    async function refresh() {
      try {
        const response = await fetch('/api/dashboard', { cache: 'no-store' });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        currentEvents = data.events;
        document.getElementById('device-count').textContent = data.devices.length;
        document.getElementById('online-count').textContent = data.devices.filter(device => device.connected).length;
        document.getElementById('threshold-value').textContent = data.threshold;
        document.querySelector('#threshold-form input').value = data.threshold;
        renderDevices(data.devices);
        renderHistory(data.events);
        draw(data.events);
      } catch (error) {
        document.getElementById('notice').textContent = `Dashboard refresh failed: ${error}`;
      }
    }

    async function submitForm(form, path) {
      const response = await fetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams(new FormData(form))
      });
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
      document.getElementById('notice').textContent = result.message;
      refresh();
    }

    document.getElementById('threshold-form').addEventListener('submit', event => {
      event.preventDefault();
      submitForm(event.currentTarget, '/api/threshold').catch(error => {
        document.getElementById('notice').textContent = error.message;
      });
    });
    document.getElementById('calibration-form').addEventListener('submit', event => {
      event.preventDefault();
      submitForm(event.currentTarget, '/api/calibration').catch(error => {
        document.getElementById('notice').textContent = error.message;
      });
    });

    const dashboard = document.getElementById('dashboard');
    let draggedWidget;
    dashboard.addEventListener('dragstart', event => {
      draggedWidget = event.target.closest('.widget');
      if (draggedWidget) draggedWidget.classList.add('dragging');
    });
    dashboard.addEventListener('dragend', () => {
      if (draggedWidget) draggedWidget.classList.remove('dragging');
      draggedWidget = null;
      localStorage.setItem('measure-dashboard-order',
        JSON.stringify([...dashboard.querySelectorAll('.widget')].map(widget => widget.dataset.widget)));
    });
    dashboard.addEventListener('dragover', event => {
      event.preventDefault();
      const target = event.target.closest('.widget');
      if (!draggedWidget || !target || target === draggedWidget) return;
      const bounds = target.getBoundingClientRect();
      target.parentNode.insertBefore(draggedWidget,
        event.clientY < bounds.top + bounds.height / 2 ? target : target.nextSibling);
    });
    try {
      const order = JSON.parse(localStorage.getItem('measure-dashboard-order') || '[]');
      order.forEach(key => {
        const widget = dashboard.querySelector(`[data-widget="${key}"]`);
        if (widget) widget.parentNode.append(widget);
      });
    } catch (_) {}
    refresh();
    setInterval(refresh, 2000);
    window.addEventListener('resize', () => draw(currentEvents));
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
  response << "X-Content-Type-Options: nosniff\r\n";
  response << "Connection: close\r\n\r\n";
  response << body;
  const std::string response_text = response.str();
  SendAll(client_fd, response_text);
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
    while (value_start < line.size() && std::isspace(
               static_cast<unsigned char>(line[value_start]))) {
      ++value_start;
    }
    return line.substr(value_start);
  }
  return "";
}

bool ReadHttpRequest(int client_fd, std::string* request_text) {
  constexpr size_t kMaxHeaderBytes = 16 * 1024;
  constexpr size_t kMaxBodyBytes = 4 * 1024;
  char buffer[1024];
  request_text->clear();

  size_t header_end = std::string::npos;
  while ((header_end = request_text->find("\r\n\r\n")) == std::string::npos) {
    ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      return false;
    }
    request_text->append(buffer, static_cast<size_t>(received));
    if (request_text->size() > kMaxHeaderBytes &&
        (header_end = request_text->find("\r\n\r\n")) == std::string::npos) {
      return false;
    }
  }
  if (header_end > kMaxHeaderBytes) return false;

  const std::string headers = request_text->substr(0, header_end);
  if (!GetHeaderValue(headers, "transfer-encoding").empty()) return false;
  const std::string content_length =
      GetHeaderValue(headers, "content-length");
  size_t body_length = 0;
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
  while (request_text->size() < request_length) {
    ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received <= 0) return false;
    request_text->append(buffer, static_cast<size_t>(received));
  }
  return true;
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

bool ParseForm(const std::string& body, std::map<std::string, std::string>* form) {
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

void HandleHttpClient(int client_fd, MeasureServiceImpl& service,
                      uint16_t http_port) {
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
    if (method != "POST") {
      SendHttpResponse(client_fd, "405 Method Not Allowed",
                       "text/plain; charset=utf-8", "Method not allowed.\n");
      return;
    }
    if (!HasAllowedOrigin(request_text.substr(0, request_text.find("\r\n\r\n")),
                          http_port)) {
      SendApiError(client_fd, "403 Forbidden",
                   "A local dashboard origin is required.");
      return;
    }

    const std::string headers =
        request_text.substr(0, request_text.find("\r\n\r\n"));
    if (GetHeaderValue(headers, "content-length").empty()) {
      SendApiError(client_fd, "400 Bad Request", "Content-Length is required.");
      return;
    }
    const size_t header_end = request_text.find("\r\n\r\n");
    const std::string body = request_text.substr(header_end + 4);
    std::map<std::string, std::string> form;
    if (!ParseForm(body, &form)) {
      SendApiError(client_fd, "400 Bad Request", "Invalid form data.");
      return;
    }
    if (path == "/api/threshold") {
      int threshold = 0;
      if (!ParseInteger(form, "threshold", &threshold) || threshold < 0 ||
          threshold > 100000) {
        SendApiError(client_fd, "400 Bad Request",
                     "Threshold must be an integer from 0 to 100000.");
        return;
      }
      service.SetThreshold(threshold);
      SendHttpResponse(client_fd, "200 OK",
                       "application/json; charset=utf-8",
                       "{\"message\":\"Fleet threshold updated.\"}");
      return;
    }
    if (path == "/api/calibration") {
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
      SendHttpResponse(client_fd, "200 OK",
                       "application/json; charset=utf-8",
                       "{\"message\":\"Calibration started.\"}");
      return;
    }
    SendApiError(client_fd, "404 Not Found", "Not found.");
    return;
  }

  if (path == "/" || path == "/index.html") {
    SendHttpResponse(client_fd, "200 OK", "text/html; charset=utf-8",
                     BuildDashboardHtml());
    return;
  }

  if (path == "/api/dashboard") {
    SendHttpResponse(client_fd, "200 OK", "application/json; charset=utf-8",
                     BuildDashboardJson(service));
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
  HttpServer(uint16_t port, MeasureServiceImpl& service)
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

    std::cout << "Fleet dashboard available at http://" << kHttpBindAddress << ":"
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
      HandleHttpClient(client_fd, service_, port_);
      close(client_fd);
    }

    CloseServerSocket();
  }

  uint16_t port_;
  MeasureServiceImpl& service_;
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
