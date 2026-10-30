#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dashboard_server.h"
#include "measure_service.h"

namespace {

class BoundSocket {
 public:
  BoundSocket() : fd_(socket(AF_INET, SOCK_STREAM, 0)) {}
  ~BoundSocket() {
    if (fd_ >= 0) close(fd_);
  }
  int fd() const { return fd_; }

 private:
  int fd_;
};

}  // namespace

TEST_CASE("invalid dashboard address fails synchronously") {
  MeasureServiceImpl service(10);
  DashboardServer dashboard(0, service, ".", "not-an-ip");
  REQUIRE_THROWS_WITH(
      dashboard.Start(),
      Catch::Matchers::ContainsSubstring("Invalid dashboard bind address"));
  REQUIRE_NOTHROW(dashboard.Stop());
}

TEST_CASE("occupied dashboard port fails synchronously and can be retried") {
  MeasureServiceImpl service(10);
  BoundSocket occupied;
  REQUIRE(occupied.fd() >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  REQUIRE(bind(occupied.fd(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == 0);
  REQUIRE(listen(occupied.fd(), 1) == 0);
  socklen_t length = sizeof(address);
  REQUIRE(getsockname(occupied.fd(), reinterpret_cast<sockaddr*>(&address),
                      &length) == 0);

  DashboardServer dashboard(ntohs(address.sin_port), service, ".");
  REQUIRE_THROWS_WITH(
      dashboard.Start(),
      Catch::Matchers::ContainsSubstring("Failed to bind dashboard server"));
  REQUIRE_THROWS_WITH(
      dashboard.Start(),
      Catch::Matchers::ContainsSubstring("Failed to bind dashboard server"));
}

TEST_CASE("dashboard starts stops and restarts on a valid address") {
  MeasureServiceImpl service(10);
  DashboardServer dashboard(0, service, ".");
  REQUIRE_NOTHROW(dashboard.Start());
  REQUIRE_NOTHROW(dashboard.Start());
  REQUIRE_NOTHROW(dashboard.Stop());
  REQUIRE_NOTHROW(dashboard.Stop());
  REQUIRE_NOTHROW(dashboard.Start());
  REQUIRE_NOTHROW(dashboard.Stop());
}
