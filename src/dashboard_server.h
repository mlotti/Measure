#ifndef MEASURE_DASHBOARD_SERVER_H
#define MEASURE_DASHBOARD_SERVER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class MeasureServiceImpl;

class DashboardServer {
 public:
  DashboardServer(uint16_t port, MeasureServiceImpl& service,
                  std::string web_root);
  ~DashboardServer();

  void Start();
  void Stop();

 private:
  void Run();
  void CloseServerSocket();

  uint16_t port_;
  MeasureServiceImpl& service_;
  std::string web_root_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> server_fd_{-1};
  std::thread thread_;
};

#endif
