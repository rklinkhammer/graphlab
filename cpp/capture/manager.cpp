#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <graphlab/terminal.hpp>
#include <openssl/evp.h>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <unistd.h>
namespace graphlab::capture {
namespace {
std::string boot() {
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  std::string s;
  f >> s;
  return s;
}
std::string command(std::vector<std::string> args) {
  auto r = runtime::process(args);
  if (r.code)
    throw runtime::Failure("capture_supervisor_failed", 503);
  while (!r.output.empty() && r.output.back() == '\n')
    r.output.pop_back();
  return r.output;
}
Json manifest(const Json &c) {
  return console::load(std::filesystem::path(c.at("directory").get<std::string>()) /
                       "manifest.json");
}
bool captured(const Json &r) { return r["topology"]["capture"]["required"] == true; }
void verify(const Json &c, const Json &s) {
  if (s.at("id") != c.at("id") || s.at("bootId") != c.at("bootId") ||
      s.at("mapping") != c.at("mapping") || s.at("runId") != c.at("runId"))
    throw runtime::Failure("capture_identity_changed", 409);
  auto invocation =
      command({"/usr/bin/systemctl", "show", c.at("unit"), "--property=InvocationID", "--value"});
  if (invocation != s.at("invocationId").get<std::string>())
    throw runtime::Failure("capture_unit_changed", 409);
}
} // namespace
Json worker_call(const Json &c, const std::string &operation, const std::string &generation,
                 const Json &params) {
  auto path =
      (std::filesystem::path(c.at("directory").get<std::string>()) / "control.sock").string();
  if (path.size() >= sizeof(sockaddr_un::sun_path))
    throw runtime::Failure("capture_socket_path_too_long");
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    throw runtime::Failure("worker_socket");
  struct Guard {
    int fd;
    ~Guard() { close(fd); }
  } guard{fd};
#ifdef SO_NOSIGPIPE
  int suppress_sigpipe = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe, sizeof(suppress_sigpipe)))
    throw runtime::Failure("worker_socket_configuration");
#endif
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strcpy(address.sun_path, path.c_str());
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
    throw runtime::Failure("capture_worker_unavailable", 503);
#ifdef __linux__
  ucred peer{};
  socklen_t len = sizeof(peer);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) || peer.uid != 0)
    throw runtime::Failure("capture_peer_denied");
#endif
  auto body = Json{
      {"nonce", c.at("nonce")},
      {"generation", generation},
      {"operation", operation},
      {"params",
       params}}.dump();
  int send_flags = 0;
#ifdef MSG_NOSIGNAL
  send_flags = MSG_NOSIGNAL;
#endif
  if (send(fd, body.data(), body.size(), send_flags) != static_cast<ssize_t>(body.size()))
    throw runtime::Failure("worker_send");
  std::string output;
  char buffer[8192];
  for (;;) {
    auto n = recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0)
      break;
    if (n < 0)
      throw runtime::Failure("worker_read");
    output.append(buffer, n);
    if (output.size() > 1024 * 1024)
      throw runtime::Failure("worker_response_limit");
  }
  auto response = Json::parse(output);
  if (response.contains("error"))
    throw runtime::Failure(response.at("error"), 409);
  return response;
}
Json plan(const Json &run, const std::filesystem::path &root) {
  Json result = Json::array();
  if (!captured(run))
    return result;
#ifndef __linux__
  (void)root;
  throw runtime::Failure("capture_requires_linux");
#else
  auto policy = run.at("capturePolicy");
  auto edges = run["topology"]["edges"].size();
  if (!edges)
    return result;
  std::filesystem::create_directories(root);
  chmod(root.c_str(), 0700);
  std::uint64_t retained = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
    if (entry.is_regular_file())
      retained += entry.file_size();
  auto budget = policy.at("runBytes").get<std::uint64_t>();
  auto reserve = policy.at("reserveBytes").get<std::uint64_t>();
  auto overhead =
      edges * 8 * 1024 * 1024; // manifests, directory entries and small-segment allocation
  struct statvfs space{};
  if (retained + budget + overhead > 8ull * 1024 * 1024 * 1024 || statvfs(root.c_str(), &space) ||
      std::uint64_t(space.f_bavail) * space.f_frsize < budget + reserve + overhead)
    throw runtime::Failure("capture_storage_reservation");
  auto per = budget / edges;
  if (per < 262144)
    throw runtime::Failure("capture_budget_per_edge");
  std::uint64_t memory_mib = edges * 64, available_kib = 0;
  for (const auto &[id, n] : run["topology"]["nodes"].items())
    if (n["kind"] == "docker" || n["kind"] == "qemu") {
      memory_mib += run["artifacts"]["workloads"][n["workload"].get<std::string>()]["contract"]
                       ["resources"]["memoryMiB"]
                           .get<std::uint64_t>();
      if (n["kind"] == "qemu")
        memory_mib += 512;
    }
  std::ifstream memory("/proc/meminfo");
  std::string line;
  while (std::getline(memory, line))
    if (line.starts_with("MemAvailable:")) {
      std::istringstream value(line.substr(13));
      value >> available_kib;
    }
  if (memory_mib * 1024 > available_kib * 4 / 5)
    throw runtime::Failure("capture_memory_reservation");
  for (const auto &r : run["resources"])
    if (r["kind"] == "edge") {
      auto endpoint = r.at("identity").at("endpoints").at(0);
      int selected = 0, index = 0;
      for (const auto &candidate : r["identity"]["endpoints"]) {
        if (candidate.value("namespace", "") == "host") {
          endpoint = candidate;
          selected = index;
          break;
        }
        ++index;
      }
      auto id = console::random_hex(12);
      auto directory = root / id;
      std::string ns = "/proc/self/ns/net", inode;
      if (endpoint.contains("containerId")) {
        auto c = runtime::docker_json(
            "GET", "/v1.52/containers/" + endpoint.at("containerId").get<std::string>() + "/json");
        if (c["Id"] != endpoint["containerId"] || c["State"]["Running"] != true)
          throw runtime::Failure("capture_container_changed");
        ns = "/proc/" + std::to_string(c["State"]["Pid"].get<int>()) + "/ns/net";
        inode = endpoint.at("namespaceInode");
      } else {
        struct stat identity{};
        if (stat(ns.c_str(), &identity))
          throw runtime::Failure("capture_namespace");
        inode = std::to_string(identity.st_ino);
      }
      Json config = {{"id", id},
                     {"runId", run["id"]},
                     {"edge", r["logical"]},
                     {"epoch", run["captureEpoch"]},
                     {"mapping", r["identity"]},
                     {"canonicalEndpoint", r["configuration"]["endpoints"][selected]},
                     {"direction", "RX: opposite endpoint to selected endpoint; TX: selected "
                                   "endpoint to opposite; individual packet direction unknown"},
                     {"namespacePath", ns},
                     {"namespaceInode", inode},
                     {"interface", endpoint["name"]},
                     {"ifindex", endpoint["ifindex"]},
                     {"bootId", boot()},
                     {"nonce", console::random_hex(32)},
                     {"generation", run["controllerGeneration"]},
                     {"directory", directory.string()},
                     {"unit", "graphlab-cap-" + id + ".service"},
                     {"snaplen", 65535},
                     {"filter", ""},
                     {"rotateBytes", policy["rotateBytes"]},
                     {"rotateSeconds", policy["rotateSeconds"]},
                     {"byteBudget", per},
                     {"reserveBytes", reserve}};
      result.push_back(config);
    }
  return result;
#endif
}
Json control(const Json &run, const std::string &operation) {
  Json observations = Json::array();
  for (const auto &c : run.value("captures", Json::array())) {
    auto directory = std::filesystem::path(c.at("directory").get<std::string>());
    if (operation == "arm") {
      if (!std::filesystem::create_directory(directory))
        throw runtime::Failure("capture_directory_conflict");
      chmod(directory.c_str(), 0700);
      // Linux libpcap cannot activate on an administratively down interface.
      // Enable only the verified capture endpoint; its peer and workload gates
      // remain held until the all-edge barrier and normal activation.
      int ns = open(c.at("namespacePath").get<std::string>().c_str(), O_RDONLY);
      if (ns < 0)
        throw runtime::Failure("capture_namespace_unavailable");
      struct NamespaceGuard {
        int fd;
        ~NamespaceGuard() { close(fd); }
      } ns_guard{ns};
      struct stat identity{};
      if (fstat(ns, &identity) || c.at("namespaceInode") != std::to_string(identity.st_ino))
        throw runtime::Failure("capture_namespace_changed");
      std::vector<std::string> prefix = {
          "/usr/bin/nsenter", "--net=/proc/self/fd/" + std::to_string(ns), "--", "/usr/sbin/ip"};
      auto query = prefix;
      query.insert(query.end(), {"-j", "link", "show"});
      auto observed = runtime::detail::lookup_link(runtime::process(query), c.at("interface"));
      if (observed.is_null() || observed.at("ifindex") != c.at("ifindex") ||
          observed.value("ifalias", "") !=
              c.at("runId").get<std::string>() + ":edge/" + c.at("edge").get<std::string>())
        throw runtime::Failure("capture_interface_changed");
      auto up = prefix;
      up.insert(up.end(), {"link", "set", "dev", c.at("interface"), "up"});
      command(up);
      auto config = c;
      config["captureEndpointEnabledAt"] = console::timestamp();
      atomic_json(directory / "config.json", config);
      auto executable =
          std::filesystem::read_symlink("/proc/self/exe").parent_path() / "lab-capture";
      struct stat binary{};
      if (lstat(executable.c_str(), &binary) || !S_ISREG(binary.st_mode) || binary.st_uid != 0 ||
          (binary.st_mode & 0022))
        throw runtime::Failure("capture_worker_binary_not_trusted");
      command(
          {"/usr/bin/systemd-run", "--quiet", "--unit=" + c.at("unit").get<std::string>(),
           "--property=Type=exec", "--property=Restart=no", "--property=KillMode=control-group",
           "--property=MemoryMax=64M", "--property=TasksMax=4",
           "--property=CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_ADMIN",
           "--property=ProtectSystem=strict", "--property=ReadWritePaths=" + directory.string(),
           "--property=LimitFSIZE=" +
               std::to_string(c.value("fileLimitBytes", c.at("byteBudget").get<std::uint64_t>())),
           "--property=NoNewPrivileges=yes", "--property=UMask=0077", executable.string(),
           "--config", (directory / "config.json").string()});
      bool ready = false;
      for (int i = 0; i < 100; ++i) {
        try {
          auto status = worker_call(c, "status", run.at("controllerGeneration"));
          verify(c, status);
          if (status["state"] == "active") {
            observations.push_back(status);
            ready = true;
            break;
          }
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!ready)
        throw runtime::Failure("capture_arm_failed");
    } else if (operation == "stop") {
      if (!std::filesystem::exists(directory))
        continue; // journaled intent before launch
      auto listed = command(
          {"/usr/bin/systemctl", "list-units", "--all", "--no-legend", "--plain", c.at("unit")});
      auto active = listed.empty() ? "inactive"
                                   : command({"/usr/bin/systemctl", "show", c.at("unit"),
                                              "--property=ActiveState", "--value"});
      if (active == "active" || active == "activating") {
        Json s;
        try {
          s = worker_call(c, "status", run.at("controllerGeneration"));
        } catch (...) {
          s = worker_call(c, "adopt", run.at("controllerGeneration"));
        }
        verify(c, s);
        worker_call(c, "stop", run.at("controllerGeneration"));
        for (int i = 0; i < 100; ++i) {
          if (manifest(c).value("state", "") == "closed")
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (manifest(c).value("state", "") != "closed")
          throw runtime::Failure("capture_finalize_timeout");
      }
      // Reap a stopped/failed unit before removing its stale control endpoint.
      if (!listed.empty()) {
        auto stopped = runtime::process({"/usr/bin/systemctl", "stop", c.at("unit")});
        if (stopped.code && !command({"/usr/bin/systemctl", "list-units", "--all", "--no-legend",
                                      "--plain", c.at("unit")})
                                 .empty())
          throw runtime::Failure("capture_stop_failed");
        runtime::detail::reap_stopped_unit(c.at("unit"));
      }
      auto stored = console::load(directory / "config.json");
      if (stored.at("id") != c.at("id") || stored.at("nonce") != c.at("nonce"))
        throw runtime::Failure("capture_ownership_conflict");
      auto socket = directory / "control.sock";
      struct stat socket_info{};
      if (lstat(socket.c_str(), &socket_info) == 0) {
        if (!S_ISSOCK(socket_info.st_mode) || socket_info.st_uid != 0 || unlink(socket.c_str()))
          throw runtime::Failure("capture_socket_cleanup_failed");
      } else if (errno != ENOENT)
        throw runtime::Failure("capture_socket_inspection_failed");
      if (std::filesystem::exists(directory / "manifest.json")) {
        auto m = manifest(c);
        if (m.value("state", "") != "closed") {
          m["state"] = "interrupted";
          m["partialFilesPossible"] = true;
        }
        observations.push_back(m);
      } else
        observations.push_back(
            {{"id", c["id"]}, {"state", "interrupted"}, {"segments", Json::array()}});
    } else {
      if (c.at("bootId") != boot())
        throw runtime::Failure("capture_prior_boot");
      Json status;
      try {
        status = worker_call(c, operation == "adopt" ? "adopt" : "status",
                             run.at("controllerGeneration"));
      } catch (...) {
        if (std::filesystem::exists(directory / "manifest.json")) {
          auto m = manifest(c);
          if (m.value("state", "") == "failed")
            throw runtime::Failure(m.value("error", "capture_worker_failed"), 503);
        }
        throw;
      }
      verify(c, status);
      if (status.at("state") != "active")
        throw runtime::Failure("required_capture_inactive");
      observations.push_back(status);
    }
  }
  return observations;
}
Json artifacts(const Json &run) {
  Json result = Json::array();
  auto captures = run.value("captureHistory", Json::array());
  for (const auto &c : run.value("captures", Json::array()))
    captures.push_back(c);
  for (const auto &c : captures) {
    auto directory = std::filesystem::path(c.at("directory").get<std::string>());
    std::set<std::string> indexed;
    try {
      auto m = manifest(c);
      for (const auto &s : m.at("segments"))
        indexed.insert(s.at("file"));
    } catch (...) {
    }
    if (std::filesystem::exists(directory))
      for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_symlink() || !entry.is_regular_file() ||
            (entry.path().extension() != ".partial" &&
             (entry.path().extension() != ".pcapng" ||
              indexed.contains(entry.path().filename().string()))))
          continue;
        auto partial = inspect_partial(entry.path());
        if (entry.path().extension() == ".pcapng")
          partial["state"] = "unindexed";
        partial["captureId"] = c["id"];
        partial["edge"] = c["edge"];
        partial["epoch"] = c["epoch"];
        result.push_back(partial);
      }
    try {
      auto m = manifest(c);
      for (auto segment : m.at("segments")) {
        segment["id"] =
            c.at("id").get<std::string>() + "-" + segment.at("sequence").get<std::string>();
        segment["captureId"] = c["id"];
        segment["edge"] = c["edge"];
        segment["epoch"] = c["epoch"];
        result.push_back(segment);
      }
    } catch (...) {
      result.push_back(
          {{"captureId", c["id"]}, {"edge", c["edge"]}, {"state", "unavailable-or-partial"}});
    }
  }
  for (auto &artifact : terminal::artifacts(run))
    result.push_back(std::move(artifact));
  return {{"items", result}};
}
Json download(const Json &run, const std::string &id, std::uint64_t offset) {
  if (id.starts_with("terminal-"))
    return terminal::download(run, id, offset);
  auto captures = run.value("captureHistory", Json::array());
  for (const auto &c : run.value("captures", Json::array()))
    captures.push_back(c);
  for (const auto &c : captures) {
    if (!id.starts_with(c.at("id").get<std::string>() + "-"))
      continue;
    auto m = manifest(c);
    for (const auto &s : m.at("segments")) {
      if (id != c.at("id").get<std::string>() + "-" + s.at("sequence").get<std::string>())
        continue;
      auto name = s.at("sequence").get<std::string>() + ".pcapng";
      if (name.find_first_not_of("0123456789.pcapng") != std::string::npos)
        throw runtime::Failure("artifact_identity");
      auto path = std::filesystem::path(c.at("directory").get<std::string>()) / name;
      int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0)
        throw runtime::Failure("artifact_unavailable", 404);
      struct Guard {
        int fd;
        ~Guard() { close(fd); }
      } guard{fd};
      struct stat info{};
      if (fstat(fd, &info) || !S_ISREG(info.st_mode) || offset > std::uint64_t(info.st_size))
        throw runtime::Failure("invalid_artifact_offset");
      unsigned char bytes[65536];
      auto n = pread(fd, bytes, sizeof(bytes), offset);
      if (n < 0)
        throw runtime::Failure("artifact_read");
      std::string encoded(4 * ((n + 2) / 3) + 1, '\0');
      auto count = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), bytes, n);
      encoded.resize(count);
      return {{"base64", encoded},
              {"offset", std::to_string(offset)},
              {"next", std::to_string(offset + n)},
              {"eof", offset + n == std::uint64_t(info.st_size)},
              {"sha256", s.at("sha256")},
              {"size", s.at("size")}};
    }
  }
  throw runtime::Failure("artifact_not_found", 404);
}
} // namespace graphlab::capture
