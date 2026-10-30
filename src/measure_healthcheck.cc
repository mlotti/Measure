#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

void WaitForSocket(int fd, short events, Clock::time_point deadline) {
  for (;;) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (remaining.count() <= 0) {
      throw std::runtime_error("Dashboard health check timed out");
    }
    pollfd socket{fd, events, 0};
    const int result = poll(&socket, 1, static_cast<int>(remaining.count()));
    if (result > 0) return;
    if (result < 0 && errno != EINTR) {
      throw std::runtime_error(std::strerror(errno));
    }
  }
}

void CheckDashboard(int fd) {
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(8080);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    if (errno != EINPROGRESS) {
      throw std::runtime_error(std::strerror(errno));
    }
    WaitForSocket(fd, POLLOUT, deadline);
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    if (error != 0) throw std::runtime_error(std::strerror(error));
  }

  const std::string request =
      "GET /api/dashboard HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
      "Connection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < request.size()) {
    WaitForSocket(fd, POLLOUT, deadline);
    const auto count = send(fd, request.data() + sent, request.size() - sent,
                            MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      throw std::runtime_error(std::strerror(errno));
    }
    if (count == 0) throw std::runtime_error("Dashboard connection closed");
    sent += static_cast<size_t>(count);
  }

  std::string status;
  while (status.size() < 128) {
    WaitForSocket(fd, POLLIN, deadline);
    char byte;
    const auto count = recv(fd, &byte, 1, 0);
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      throw std::runtime_error(std::strerror(errno));
    }
    if (count == 0) throw std::runtime_error("Missing HTTP status line");
    status += byte;
    if (byte == '\n') {
      if ((status.compare(0, 13, "HTTP/1.1 200 ") == 0 ||
           status.compare(0, 13, "HTTP/1.0 200 ") == 0) &&
          status.size() >= 2 && status[status.size() - 2] == '\r') {
        return;
      }
      throw std::runtime_error("Unexpected dashboard status: " + status);
    }
  }
  throw std::runtime_error("Dashboard HTTP status line is too long");
}

}  // namespace

int main() {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    std::cerr << "Health check failed: " << std::strerror(errno) << '\n';
    return 1;
  }
  try {
    CheckDashboard(fd);
    close(fd);
    return 0;
  } catch (const std::exception& error) {
    close(fd);
    std::cerr << "Health check failed: " << error.what() << '\n';
    return 1;
  }
}
