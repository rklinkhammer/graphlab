#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>

namespace lab_support::detail {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
// All descriptors are nonblocking. A readiness notification never authorizes I/O
// by itself: recheck the absolute deadline after waking, immediately before I/O.
inline bool wait_ready(int fd, short events, Deadline end) {
  while (Clock::now() < end) {
    auto remaining = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now()).count();
    if (remaining <= 0)
      return false;
    pollfd p{fd, events, 0};
    int rc = poll(&p, 1, static_cast<int>(std::min<std::int64_t>(20, remaining)));
    if (Clock::now() >= end)
      return false;
    if (rc > 0)
      return (p.revents & events) != 0;
    if (rc < 0 && errno != EINTR)
      return false;
  }
  return false;
}
inline bool record_io(int fd, char *bytes, bool write, Deadline traffic_deadline) {
  const auto end = std::min(traffic_deadline, Clock::now() + std::chrono::milliseconds(500));
  std::size_t offset = 0;
  while (offset < 16) {
    if (!wait_ready(fd, write ? POLLOUT : POLLIN, end) || Clock::now() >= end)
      return false;
    auto n =
        write ? send(fd, bytes + offset, 16 - offset, 0) : recv(fd, bytes + offset, 16 - offset, 0);
    if (n > 0)
      offset += n;
    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      return false;
  }
  return Clock::now() < end;
}
} // namespace lab_support::detail
