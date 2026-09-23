#include <cstring>

#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <iostream>
#include <linux/capability.h>
#include <net/if.h>
#include <pcap/pcap.h>
#include <poll.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
using namespace graphlab;
using lab_support::Json;
namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
std::string boot() {
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  std::string s;
  f >> s;
  return s;
}
struct FD {
  int fd = -1;
  ~FD() {
    if (fd >= 0)
      close(fd);
  }
};
Json stats(pcap_t *p) {
  pcap_stat s{};
  if (pcap_stats(p, &s))
    return {{"received", nullptr}, {"dropped", nullptr}, {"interfaceDropped", nullptr}};
  return {{"received", s.ps_recv},
          {"dropped", s.ps_drop},
          {"interfaceDropped", nullptr},
          {"source", "libpcap-cumulative"}};
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3 || std::string(argv[1]) != "--config" || geteuid() != 0)
    return 2;
  Json state;
  std::filesystem::path directory = std::filesystem::path(argv[2]).parent_path();
  pcap_t *pcap = nullptr;
  bool fresh_start = false;
  std::unique_ptr<capture::Writer> writer;
  auto persist = [&] {
    state["heartbeatAt"] = console::timestamp();
    if (writer)
      state["segments"] = writer->segments();
    capture::atomic_json(directory / "manifest.json", state);
  };
  try {
    umask(0077);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    auto config = console::load(argv[2]);
    if (config.at("bootId") != boot())
      throw runtime::Failure("capture_prior_boot");
    FD started{open((directory / "worker.started").c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (started.fd < 0)
      throw runtime::Failure("capture_identity_already_used");
    if (fsync(started.fd))
      throw runtime::Failure("capture_start_marker_sync");
    fresh_start = true;
    auto invocation = getenv("INVOCATION_ID");
    if (!invocation || !*invocation)
      throw runtime::Failure("capture_requires_independent_unit");
    auto socket_path = (directory / "control.sock").string();
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path))
      throw runtime::Failure("capture_socket_path_too_long");
    FD server{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, socket_path.c_str());
    if (bind(server.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        listen(server.fd, 8))
      throw runtime::Failure("capture_control_bind");
    state = {{"apiVersion", "graphlab.capture/v1"},
             {"id", config.at("id")},
             {"runId", config.at("runId")},
             {"edge", config.at("edge")},
             {"epoch", config.at("epoch")},
             {"bootId", boot()},
             {"invocationId", invocation},
             {"pid", getpid()},
             {"generation", config.at("generation")},
             {"mapping", config.at("mapping")},
             {"state", "starting"},
             {"sourceSeq", "0"},
             {"libpcap", pcap_lib_version()},
             {"segments", Json::array()},
             {"exclusions", "interface creation before activation; per-packet direction "
                            "unavailable; offloads unknown"}};
    auto ns = config.at("namespacePath").get<std::string>();
    FD network{open(ns.c_str(), O_RDONLY | O_CLOEXEC)};
    struct stat identity{};
    if (network.fd < 0 || fstat(network.fd, &identity) ||
        std::to_string(identity.st_ino) != config.at("namespaceInode").get<std::string>())
      throw runtime::Failure("capture_namespace_changed");
    if (ns != "/proc/self/ns/net" && setns(network.fd, CLONE_NEWNET))
      throw runtime::Failure("capture_setns");
    auto name = config.at("interface").get<std::string>();
    if (if_nametoindex(name.c_str()) != config.at("ifindex").get<unsigned>())
      throw runtime::Failure("capture_interface_changed");
    char error[PCAP_ERRBUF_SIZE]{};
    pcap = pcap_create(name.c_str(), error);
    if (!pcap)
      throw runtime::Failure("pcap_create");
    if (pcap_set_snaplen(pcap, config.at("snaplen")) || pcap_set_promisc(pcap, 1) ||
        pcap_set_timeout(pcap, 50) || pcap_set_buffer_size(pcap, 16 * 1024 * 1024) ||
        pcap_set_immediate_mode(pcap, 1))
      throw runtime::Failure("pcap_configuration");
    auto activated = pcap_activate(pcap);
    state["activationStatus"] = activated;
    state["activationMessage"] = pcap_geterr(pcap);
    state["captureEndpointEnabledAt"] = config.value("captureEndpointEnabledAt", "");
    if (activated != 0)
      throw runtime::Failure("pcap_activation_warning_or_failure");
    if (pcap_datalink(pcap) != DLT_EN10MB)
      throw runtime::Failure("capture_requires_ethernet");
    if (pcap_setnonblock(pcap, 1, error))
      throw runtime::Failure("pcap_nonblock");
    auto filter = config.at("filter").get<std::string>();
    if (!filter.empty()) {
      bpf_program program{};
      if (pcap_compile(pcap, &program, filter.c_str(), 1, PCAP_NETMASK_UNKNOWN))
        throw runtime::Failure("capture_filter_compile");
      auto result = pcap_setfilter(pcap, &program);
      pcap_freecode(&program);
      if (result)
        throw runtime::Failure("capture_filter_install");
    }
    writer = std::make_unique<capture::Writer>(directory, config);
    __user_cap_header_struct cap_header{_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct cap_data[2]{};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || syscall(SYS_capset, &cap_header, cap_data))
      throw runtime::Failure("capture_privilege_drop");
    writer->open();
    state["state"] = "active";
    state["activatedAt"] = console::timestamp();
    state["filterInstalled"] = true;
    state["outputReady"] = true;
    persist();
    std::uint64_t seq = 0;
    auto heartbeat = std::chrono::steady_clock::now(), opened = heartbeat;
    auto drain = [&] {
      for (int i = 0; i < 256; ++i) {
        if (writer->bytes() >= config.at("rotateBytes").get<std::uint64_t>())
          break;
        pcap_pkthdr *header = nullptr;
        const unsigned char *packet = nullptr;
        auto result = pcap_next_ex(pcap, &header, &packet);
        if (result == 0)
          break;
        if (result < 0)
          throw runtime::Failure("capture_read_failed");
        writer->packet(std::uint64_t(header->ts.tv_sec) * 1000000 + header->ts.tv_usec,
                       {packet, header->caplen}, header->len);
      }
    };
    auto flush_pending = [&] {
      // A bounded drain window includes packets still being retired by the
      // kernel capture ring. This is not a zero-drop/delivery guarantee.
      auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      do {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < until);
    };
    while (!stopping) {
      drain();
      auto now = std::chrono::steady_clock::now();
      if (now - heartbeat >= std::chrono::seconds(1)) {
        struct statvfs space{};
        if (statvfs(directory.c_str(), &space) ||
            std::uint64_t(space.f_bavail) * space.f_frsize <
                config.at("reserveBytes").get<std::uint64_t>())
          throw runtime::Failure("capture_free_space_reserve");
        if (if_nametoindex(name.c_str()) != config.at("ifindex").get<unsigned>())
          throw runtime::Failure("capture_mapping_lost");
        state["statistics"] = stats(pcap);
        state["sourceSeq"] = std::to_string(++seq);
        writer->sync();
        persist();
        heartbeat = now;
      }
      if (writer->bytes() >= config.at("rotateBytes").get<std::uint64_t>() ||
          now - opened >= std::chrono::seconds(config.at("rotateSeconds").get<int>())) {
        writer->close(stats(pcap));
        persist();
        if (writer->total() + 2 * config.at("snaplen").get<std::uint64_t>() + 4096 >=
            config.at("byteBudget").get<std::uint64_t>())
          throw runtime::Failure("capture_quota_exhausted");
        writer->open();
        opened = now;
      }
      // Wake for packet readiness as well as control commands. Waiting only on
      // the control socket throttles a quiet worker to one drain every 10 ms,
      // which can overflow the kernel ring even at modest packet rates.
      pollfd events[2] = {{server.fd, POLLIN, 0}, {pcap_get_selectable_fd(pcap), POLLIN, 0}};
      poll(events, 2, 10);
      if (!(events[0].revents & POLLIN))
        continue;
      FD client{accept4(server.fd, nullptr, nullptr, SOCK_CLOEXEC)};
      if (client.fd < 0)
        continue;
      ucred peer{};
      socklen_t size = sizeof(peer);
      if (getsockopt(client.fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != 0)
        continue;
      timeval timeout{0, 100000};
      setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      char input[4096];
      auto n = recv(client.fd, input, sizeof(input), 0);
      Json response;
      try {
        if (n <= 0)
          throw runtime::Failure("invalid_worker_request");
        auto request = Json::parse(std::string(input, n));
        if (request.at("nonce") != config.at("nonce"))
          throw runtime::Failure("worker_auth_denied");
        auto generation = request.at("generation").get<std::string>(),
             operation = request.at("operation").get<std::string>();
        if (operation == "adopt") {
          if (std::stoull(generation) <= std::stoull(state.at("generation").get<std::string>()))
            throw runtime::Failure("stale_controller");
          state["generation"] = generation;
          persist();
        } else if (generation != state.at("generation").get<std::string>())
          throw runtime::Failure("stale_controller");
        if (operation == "stop")
          stopping = 1;
        else if (operation == "rotate") {
          flush_pending();
          writer->close(stats(pcap));
          persist();
          writer->open();
          opened = now;
        } else if (operation != "status" && operation != "adopt")
          throw runtime::Failure("unsupported_worker_operation");
        response = state;
      } catch (const std::exception &e) {
        response = {{"error", e.what()}};
      }
      auto output = response.dump();
      send(client.fd, output.data(), output.size(), MSG_NOSIGNAL);
    }
    flush_pending();
    writer->close(stats(pcap));
    state["statistics"] = stats(pcap);
    state["state"] = "closed";
    state["closedAt"] = console::timestamp();
    persist();
    pcap_close(pcap);
    unlink((directory / "control.sock").c_str());
    return 0;
  } catch (const std::exception &e) {
    try {
      if (writer && pcap)
        writer->close(stats(pcap));
    } catch (...) {
    }
    state["state"] = "failed";
    state["error"] = e.what();
    state["partialFilesPossible"] = true;
    try {
      if (fresh_start)
        persist();
    } catch (...) {
    }
    if (pcap)
      pcap_close(pcap);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
