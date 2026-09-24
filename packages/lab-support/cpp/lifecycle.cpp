#include "record_io.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iostream>
#include <lab_support/lifecycle.hpp>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
namespace lab_support {
namespace {
using Json = nlohmann::json;
constexpr const char *socket_path = "/run/graphlab-node.sock";
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
struct FD {
  int value = -1;
  ~FD() {
    if (value >= 0)
      close(value);
  }
};
sockaddr_un address() {
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, socket_path, sizeof(a.sun_path) - 1);
  return a;
}
sockaddr_in data_address() {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  ifaddrs *list = nullptr;
  if (getifaddrs(&list))
    throw std::runtime_error("interfaces_unavailable");
  for (auto entry = list; entry; entry = entry->ifa_next)
    if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_INET &&
        std::string(entry->ifa_name) == "data0")
      result = *reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
  freeifaddrs(list);
  if (result.sin_addr.s_addr == 0)
    throw std::runtime_error("data0_has_no_address");
  return result;
}
std::string probe(const std::string &ip) {
  FD socket{::socket(AF_INET, SOCK_DGRAM, 0)};
  if (socket.value < 0 || fcntl(socket.value, F_SETFL, O_NONBLOCK))
    throw std::runtime_error("probe_socket");
  auto local = data_address();
  local.sin_port = 0;
  if (bind(socket.value, reinterpret_cast<sockaddr *>(&local), sizeof(local)))
    throw std::runtime_error("data_bind_failed");
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(49000);
  if (inet_pton(AF_INET, ip.c_str(), &remote.sin_addr) != 1)
    throw std::runtime_error("invalid_address");
  if (connect(socket.value, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)))
    throw std::runtime_error("probe_connect");
  std::string payload = "graphlab-probe-" + std::to_string(getpid());
  send(socket.value, payload.data(), payload.size(), 0);
  pollfd p{socket.value, POLLIN, 0};
  if (poll(&p, 1, 1200) <= 0)
    return "timeout";
  char data[128];
  auto count = recv(socket.value, data, sizeof(data), 0);
  return count == static_cast<ssize_t>(payload.size()) && std::string(data, count) == payload
             ? "received"
             : "timeout";
}
using detail::record_io;
std::string stream_probe(const std::string &ip, char stream, detail::Deadline traffic_deadline) {
  FD fd{socket(AF_INET, SOCK_STREAM, 0)};
  if (fd.value < 0 || fcntl(fd.value, F_SETFL, O_NONBLOCK))
    throw std::runtime_error("probe_socket");
  auto local = data_address();
  local.sin_port = 0;
  if (bind(fd.value, reinterpret_cast<sockaddr *>(&local), sizeof(local)))
    throw std::runtime_error("data_bind_failed");
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(49001);
  if (inet_pton(AF_INET, ip.c_str(), &remote.sin_addr) != 1)
    throw std::runtime_error("invalid_address");
  if (detail::Clock::now() >= traffic_deadline)
    return "timeout";
  if (connect(fd.value, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) &&
      errno != EINPROGRESS)
    return "timeout";
  if (!detail::wait_ready(
          fd.value, POLLOUT,
          std::min(traffic_deadline, detail::Clock::now() + std::chrono::milliseconds(500))))
    return "timeout";
  int error = 0;
  socklen_t size = sizeof(error);
  if (getsockopt(fd.value, SOL_SOCKET, SO_ERROR, &error, &size) || error)
    return "timeout";
  char bytes[16];
  std::memset(bytes, stream, sizeof(bytes));
  if (!record_io(fd.value, bytes, true, traffic_deadline) ||
      !record_io(fd.value, bytes, false, traffic_deadline))
    return "timeout";
  for (char c : bytes)
    if (c != stream)
      return "mismatch";
  return "received";
}
} // namespace
int run_node(int argc, char **argv, const char *application) {
  return run_node(argc, argv, application, false);
}
int run_node(int argc, char **argv, const char *application, bool application_telemetry) {
  return run_node(argc, argv, application, application_telemetry, false);
}
int run_node(int argc, char **argv, const char *application, bool application_telemetry,
             bool edge_telemetry) {
  try {
    if (argc >= 3 && std::string(argv[1]) == "control") {
      std::string command = argv[2];
      if (argc == 4)
        command += " " + std::string(argv[3]);
      FD client{socket(AF_UNIX, SOCK_STREAM, 0)};
      auto a = address();
      if (connect(client.value, reinterpret_cast<sockaddr *>(&a), sizeof(a)))
        throw std::runtime_error("gate_unavailable");
      command += '\n';
      send(client.value, command.data(), command.size(), 0);
      std::string response;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
      while (!response.ends_with('\n')) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        pollfd p{client.value, POLLIN, 0};
        if (remaining <= 0 || poll(&p, 1, static_cast<int>(remaining)) <= 0)
          throw std::runtime_error("gate_timeout");
        char buffer[4096];
        auto n = recv(client.value, buffer, sizeof(buffer), 0);
        if (n <= 0)
          throw std::runtime_error("gate_closed");
        if (response.size() + n > 4096)
          throw std::runtime_error("gate_response_limit");
        response.append(buffer, n);
      }
      auto result = Json::parse(response);
      std::cout << result.dump() << '\n';
      return result.contains("error") ? 2 : 0;
    }
    if (argc != 1)
      throw std::runtime_error("invalid_arguments");
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    FD server{socket(AF_UNIX, SOCK_STREAM, 0)};
    auto a = address();
    if (bind(server.value, reinterpret_cast<sockaddr *>(&a), sizeof(a)) || listen(server.value, 8))
      throw std::runtime_error("gate_bind");
    bool released = false;
    bool leased = false;
    auto deadline = std::chrono::steady_clock::time_point::min();
    std::uint64_t packets = 0, sent = 0, received_bytes = 0, sent_bytes = 0, errors = 0,
                  rejected = 0, backpressure = 0, sequence = 0, latency_sum = 0;
    std::array<std::uint64_t, 8> latency_buckets{};
    constexpr std::uint64_t latency_bounds[] = {10000,   50000,   100000,  500000,
                                                1000000, 5000000, 10000000};
    const auto began = std::chrono::steady_clock::now();
    std::string epoch;
    if (application_telemetry) {
      std::random_device random;
      for (int i = 0; i < 32; ++i)
        epoch += "0123456789abcdef"[random() & 15];
    }
    struct Stream {
      std::uint64_t received = 0, bytes = 0, errors = 0, rejected = 0, connections = 0, count = 0,
                    sum = 0;
      std::array<std::uint64_t, 8> buckets{};
    };
    std::array<Stream, 2> streams{};
    FD tcp;
    FD data;
    auto expire = [&] {
      if (leased && std::chrono::steady_clock::now() >= deadline) {
        released = false;
        leased = false;
        if (tcp.value >= 0) {
          close(tcp.value);
          tcp.value = -1;
        }
        if (data.value >= 0) {
          close(data.value);
          data.value = -1;
        }
      }
    };
    while (!stopped) {
      expire();
      pollfd p[3] = {{server.value, POLLIN, 0}, {data.value, POLLIN, 0}, {tcp.value, POLLIN, 0}};
      if (poll(p, 3, 100) < 0)
        continue;
      expire();
      if (released && tcp.value >= 0 && (p[2].revents & POLLIN)) {
        FD peer{accept(tcp.value, nullptr, nullptr)};
        if (peer.value >= 0 && !fcntl(peer.value, F_SETFL, O_NONBLOCK)) {
          const auto traffic_deadline = leased ? deadline : detail::Deadline::max();
          char bytes[16];
          if (record_io(peer.value, bytes, false, traffic_deadline) &&
              (bytes[0] == 'A' || bytes[0] == 'B')) {
            auto started = std::chrono::steady_clock::now();
            auto &stream = streams[bytes[0] == 'A' ? 0 : 1];
            ++stream.connections;
            ++stream.received;
            stream.bytes += 16;
            bool valid = true;
            for (char c : bytes)
              valid &= c == bytes[0];
            if (!valid)
              ++stream.rejected;
            else if (record_io(peer.value, bytes, true, traffic_deadline)) {
              auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
              ++stream.count;
              stream.sum += ns;
              std::size_t bucket = 0;
              while (bucket < 7 && static_cast<std::uint64_t>(ns) > latency_bounds[bucket])
                ++bucket;
              ++stream.buckets[bucket];
            } else
              ++stream.errors;
          }
        }
      }
      expire(); // TCP I/O may have consumed the remaining traffic lease.
      if (p[1].revents & POLLIN) {
        char buffer[1500];
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        auto n = recvfrom(data.value, buffer, sizeof(buffer), application_telemetry ? MSG_TRUNC : 0,
                          reinterpret_cast<sockaddr *>(&peer), &len);
        if (released && (n > 0 || (application_telemetry && n == 0))) {
          const auto received = std::chrono::steady_clock::now();
          ++packets;
          received_bytes += static_cast<std::uint64_t>(n);
          if (n > static_cast<ssize_t>(sizeof(buffer))) {
            ++rejected;
          } else if (sendto(data.value, buffer, n, 0, reinterpret_cast<sockaddr *>(&peer), len) ==
                     n) {
            ++sent;
            sent_bytes += n;
            const auto elapsed =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - received)
                                               .count());
            latency_sum += elapsed;
            std::size_t bucket = 0;
            while (bucket < 7 && elapsed > latency_bounds[bucket])
              ++bucket;
            ++latency_buckets[bucket];
          } else {
            ++errors;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              ++backpressure;
          }
        }
      }
      if (!(p[0].revents & POLLIN))
        continue;
      FD client{accept(server.value, nullptr, nullptr)};
      if (client.value < 0)
        continue;
      pollfd input{client.value, POLLIN, 0};
      if (poll(&input, 1, 500) <= 0)
        continue;
      char buffer[128];
      auto n = recv(client.value, buffer, sizeof(buffer), 0);
      if (n <= 0)
        continue;
      std::string command(buffer, n);
      if (!command.ends_with('\n'))
        continue;
      command.pop_back();
      Json result;
      try {
        expire();
        if (command == "renew-lease") {
          if (!released || !leased)
            throw std::runtime_error("lease_expired_or_not_armed");
          deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        } else if (command == "release" || command == "release-lease") {
          leased = command == "release-lease";
          deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          released = true;
          if (data.value < 0) {
            try {
              auto local = data_address();
              local.sin_port = htons(49000);
              data.value = socket(AF_INET, SOCK_DGRAM, 0);
              if (data.value < 0 || fcntl(data.value, F_SETFL, O_NONBLOCK))
                throw std::runtime_error("data_socket");
              if (bind(data.value, reinterpret_cast<sockaddr *>(&local), sizeof(local))) {
                close(data.value);
                data.value = -1;
                throw std::runtime_error("data_bind_failed");
              }
            } catch (const std::exception
                         &) { /* Isolated nodes have no data address. Gate still functions. */
              if (data.value >= 0) {
                close(data.value);
                data.value = -1;
              }
            }
          }
          if (edge_telemetry && tcp.value < 0) {
            auto local = data_address();
            local.sin_port = htons(49001);
            tcp.value = socket(AF_INET, SOCK_STREAM, 0);
            int reuse = 1;
            setsockopt(tcp.value, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (tcp.value < 0 || fcntl(tcp.value, F_SETFL, O_NONBLOCK) ||
                bind(tcp.value, reinterpret_cast<sockaddr *>(&local), sizeof(local)) ||
                listen(tcp.value, 8)) {
              if (tcp.value >= 0)
                close(tcp.value);
              tcp.value = -1;
              throw std::runtime_error("stream_listener_failed");
            }
          }
        } else if (command == "quiesce") {
          if (tcp.value >= 0) {
            close(tcp.value);
            tcp.value = -1;
          }
          released = false;
          leased = false;
          if (data.value >= 0) {
            close(data.value);
            data.value = -1;
          }
        } else if (command.starts_with("probe-alpha ") || command.starts_with("probe-beta ")) {
          if (!released)
            throw std::runtime_error("gate_held");
          result["probe"] = stream_probe(command.substr(command.find(' ') + 1),
                                         command.starts_with("probe-alpha") ? 'A' : 'B',
                                         leased ? deadline : detail::Deadline::max());
        } else if (command.starts_with("probe ")) {
          if (!released)
            throw std::runtime_error("gate_held");
          result["probe"] = probe(command.substr(6));
        } else if (command != "status")
          throw std::runtime_error("unsupported_control");
      } catch (const std::exception &e) {
        result["error"] = e.what();
      }
      expire();
      result["apiVersion"] = "graphlab.gate/v1";
      result["protocolMinor"] = node_gate_minor;
      result["capabilities"] = Json::array({"quiesce", "traffic-lease"});
      if (application_telemetry)
        result["capabilities"].push_back("application-telemetry/v1");
      result["state"] = released ? "released" : "held";
      result["application"] = application;
      result["echoPackets"] = std::to_string(packets);
      if (application_telemetry && command == "status") {
        Json buckets = Json::array();
        for (auto n : latency_buckets)
          buckets.push_back(std::to_string(n));
        result["applicationTelemetry"] = {
            {"apiVersion", "graphlab.application-telemetry/v1"},
            {"stream", "udp-echo"},
            {"epoch", epoch},
            {"sequence", std::to_string(++sequence)},
            {"elapsedNs", std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - began)
                                             .count())},
            {"counters",
             {{"sentMessages", std::to_string(sent)},
              {"receivedMessages", std::to_string(packets)},
              {"sentPayloadBytes", std::to_string(sent_bytes)},
              {"receivedPayloadBytes", std::to_string(received_bytes)},
              {"errors", std::to_string(errors)},
              {"rejectedMessages", std::to_string(rejected)},
              {"backpressureEvents", std::to_string(backpressure)}}},
            {"latency",
             {{"kind", "local-service-time"},
              {"count", std::to_string(sent)},
              {"sumNs", std::to_string(latency_sum)},
              {"buckets", buckets}}}};
      }
      if (edge_telemetry && command == "status") {
        result["capabilities"].push_back("application-edge-telemetry/v1");
        result["applicationEdgeTelemetry"] = Json::array();
        for (int i = 0; i < 2; i++) {
          auto &stream = streams[i];
          Json buckets = Json::array();
          for (auto n : stream.buckets)
            buckets.push_back(std::to_string(n));
          result["applicationEdgeTelemetry"].push_back(
              {{"apiVersion", "graphlab.application-edge-telemetry/v1"},
               {"edge", i == 0 ? "alpha" : "beta"},
               {"endpoint", "target"},
               {"stream", i == 0 ? "alpha" : "beta"},
               {"epoch", epoch},
               {"sequence", std::to_string(sequence)},
               {"elapsedNs", result["applicationTelemetry"]["elapsedNs"]},
               {"counters",
                {{"sentMessages", nullptr},
                 {"sentPayloadBytes", nullptr},
                 {"receivedMessages", std::to_string(stream.received)},
                 {"receivedPayloadBytes", std::to_string(stream.bytes)},
                 {"errors", std::to_string(stream.errors)},
                 {"rejectedMessages", std::to_string(stream.rejected)},
                 {"backpressureEvents", nullptr},
                 {"backpressureNs", nullptr},
                 {"reconnects", std::to_string(stream.connections ? stream.connections - 1 : 0)}}},
               {"latency",
                {{"kind", "local-service-time"},
                 {"count", std::to_string(stream.count)},
                 {"sumNs", std::to_string(stream.sum)},
                 {"buckets", buckets}}}});
        }
      }
      result["dataReady"] = data.value >= 0;
      result["leaseActive"] = leased && released;
      result["leaseMilliseconds"] =
          leased ? std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 deadline - std::chrono::steady_clock::now())
                                                 .count())
                 : 0;
      auto out = result.dump() + "\n";
      send(client.value, out.data(), out.size(), 0);
    }
    unlink(socket_path);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
} // namespace lab_support
