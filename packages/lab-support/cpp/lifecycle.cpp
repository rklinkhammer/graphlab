#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iostream>
#include <lab_support/lifecycle.hpp>
#include <nlohmann/json.hpp>
#include <poll.h>
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
} // namespace
int run_node(int argc, char **argv, const char *application) {
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
      pollfd p{client.value, POLLIN, 0};
      if (poll(&p, 1, 2500) <= 0)
        throw std::runtime_error("gate_timeout");
      char buffer[4096];
      auto n = recv(client.value, buffer, sizeof(buffer), 0);
      if (n <= 0)
        throw std::runtime_error("gate_closed");
      auto result = Json::parse(std::string(buffer, n));
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
    std::uint64_t packets = 0;
    FD data;
    auto expire = [&] {
      if (leased && std::chrono::steady_clock::now() >= deadline) {
        released = false;
        leased = false;
        if (data.value >= 0) {
          close(data.value);
          data.value = -1;
        }
      }
    };
    while (!stopped) {
      expire();
      pollfd p[2] = {{server.value, POLLIN, 0}, {data.value, POLLIN, 0}};
      if (poll(p, 2, 100) < 0)
        continue;
      expire();
      if (p[1].revents & POLLIN) {
        char buffer[1500];
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        auto n = recvfrom(data.value, buffer, sizeof(buffer), 0,
                          reinterpret_cast<sockaddr *>(&peer), &len);
        if (released && n > 0) {
          sendto(data.value, buffer, n, 0, reinterpret_cast<sockaddr *>(&peer), len);
          ++packets;
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
        } else if (command == "quiesce") {
          released = false;
          leased = false;
          if (data.value >= 0) {
            close(data.value);
            data.value = -1;
          }
        } else if (command.starts_with("probe ")) {
          if (!released)
            throw std::runtime_error("gate_held");
          result["probe"] = probe(command.substr(6));
        } else if (command != "status")
          throw std::runtime_error("unsupported_control");
      } catch (const std::exception &e) {
        result["error"] = e.what();
      }
      result["apiVersion"] = "graphlab.gate/v1";
      result["protocolMinor"] = node_gate_minor;
      result["capabilities"] = Json::array({"quiesce", "traffic-lease"});
      result["state"] = released ? "released" : "held";
      result["application"] = application;
      result["echoPackets"] = std::to_string(packets);
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
