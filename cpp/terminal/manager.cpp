#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <graphlab/terminal.hpp>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __linux__
#include <csignal>
#include <sys/syscall.h>
#endif
namespace graphlab::terminal {
int connect_unix(const std::filesystem::path &p) {
  auto s = p.string();
  if (s.size() >= sizeof(sockaddr_un::sun_path))
    throw runtime::Failure("socket_path_limit");
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    throw runtime::Failure("socket_unavailable");
  if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
    close(fd);
    throw runtime::Failure("socket_configuration");
  }
  timeval t{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#ifdef SO_NOSIGPIPE
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strcpy(a.sun_path, s.c_str());
  if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a))) {
    close(fd);
    throw runtime::Failure("socket_unavailable");
  }
  return fd;
}
Json request(const Json &d, const std::string &op, const std::string &g, Json p) {
  auto s = capture::worker_call(d, op, g, p);
  if (s.contains("id") && (s["id"] != d["id"] || s["bootId"] != d["bootId"]))
    throw runtime::Failure("session_identity_changed");
  return s;
}
Json descriptor(const Json &r, const std::string &node, const std::filesystem::path &root,
                const std::string &kind) {
  auto id = console::random_hex(12);
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  std::string boot;
  f >> boot;
  return {{"id", id},
          {"runId", r["id"]},
          {"node", node},
          {"kind", kind},
          {"directory", (root / id).string()},
          {"unit", "graphlab-terminal-" + id + ".service"},
          {"nonce", console::random_hex(32)},
          {"generation", r["controllerGeneration"]},
          {"bootId", boot},
          {"recordInput", false},
          {"byteBudget", 64 * 1024 * 1024},
          {"createdAt", console::timestamp()}};
}
void launch(const Json &d) {
#ifdef __linux__
  auto dir = std::filesystem::path(d["directory"].get<std::string>());
  std::filesystem::create_directories(dir.parent_path());
  chmod(dir.parent_path().c_str(), 0700);
  struct statvfs space{};
  if (statvfs(dir.parent_path().c_str(), &space) ||
      std::uint64_t(space.f_bavail) * space.f_frsize <
          d["byteBudget"].get<std::uint64_t>() + 1024ull * 1024 * 1024)
    throw runtime::Failure("recording_storage_reservation");
  if (!d.value("preparedDirectory", false) && !std::filesystem::create_directory(dir))
    throw runtime::Failure("session_directory_exists");
  chmod(dir.c_str(), 0700);
  capture::atomic_json(dir / "config.json", d);
  auto exe = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "lab-terminal";
  struct stat st{};
  if (lstat(exe.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022))
    throw runtime::Failure("terminal_binary_not_trusted");
  std::vector<std::string> command = {
      "/usr/bin/systemd-run",
      "--quiet",
      "--unit=" + d["unit"].get<std::string>(),
      "--property=Type=exec",
      "--property=Restart=no",
      "--property=KillMode=control-group",
      "--property=TimeoutStopSec=5",
      "--property=NoNewPrivileges=yes",
      "--property=UMask=0077",
      "--property=ProtectSystem=strict",
      "--property=ReadWritePaths=" + dir.string(),
      "--property=InaccessiblePaths=-/run/docker.sock -/var/run/docker.sock",
      "--property=MemoryMax=" + std::to_string(d.value("memoryMiB", 128) * 1024ull * 1024),
  };
  if (d["kind"] == "docker")
    command.push_back("--property=ExecStopPost=+" + exe.string() + " --cleanup " +
                      (dir / "config.json").string());
  command.insert(command.end(), {exe.string(), "--config", (dir / "config.json").string()});
  auto result = runtime::process(command);
  if (result.code)
    throw runtime::Failure("terminal_launch_failed");
#else
  (void)d;
  throw runtime::Failure("terminal_requires_linux");
#endif
}
namespace {
struct SessionLock {
  int fd;
  explicit SessionLock(const std::filesystem::path &dir) {
    fd = open((dir / "exec.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
      throw runtime::Failure("exec_lock_unavailable");
    if (flock(fd, LOCK_EX)) {
      close(fd);
      throw runtime::Failure("exec_lock_failed");
    }
  }
  ~SessionLock() { close(fd); }
};
} // namespace
void cleanup_docker(const Json &d) {
  if (d["kind"] != "docker")
    return;
  auto dir = std::filesystem::path(d["directory"].get<std::string>());
#ifdef __linux__
  SessionLock lock(dir);
  // No exec process can survive a host boot. Do not query a retired Docker exec
  // ID or signal a PID that may have been reused in the new boot.
  std::ifstream boot_file("/proc/sys/kernel/random/boot_id");
  std::string current_boot;
  boot_file >> current_boot;
  if (current_boot.empty() || d.value("bootId", "").empty())
    throw runtime::Failure("session_boot_identity_unavailable");
  if (d["bootId"] != current_boot)
    return;
  if (d["kind"] == "docker" && std::filesystem::exists(dir / "exec.json")) {
    auto exec = console::load(dir / "exec.json");
    auto path = "/v1.52/exec/" + exec["id"].get<std::string>() + "/json";
    auto response = runtime::docker_request("GET", path);
    // Removing the parent container retires its exec IDs. Confirmed absence is
    // already-clean, including a second recovery after interrupted creation.
    if (response.status == 404)
      return;
    if (response.status != 200)
      throw runtime::Failure("exec_inspection_failed");
    auto info = Json::parse(response.body);
    if (info["ContainerID"] != d["containerId"])
      throw runtime::Failure("exec_identity_changed");
    if (info["Running"] == true) {
      int pid = info["Pid"];
      int fd = syscall(SYS_pidfd_open, pid, 0);
      if (fd < 0) {
        if (errno == ESRCH) {
          auto exited = runtime::docker_json("GET", path);
          if (exited["ContainerID"] == d["containerId"] && exited["Running"] == false)
            return;
        }
        throw runtime::Failure("exec_pidfd_unavailable");
      }
      auto checked = runtime::docker_json("GET", path);
      if (checked["ContainerID"] == d["containerId"] && checked["Running"] == false) {
        close(fd);
        return;
      }
      if (checked["ContainerID"] != d["containerId"] || checked["Pid"] != pid ||
          checked["Running"] != true) {
        close(fd);
        throw runtime::Failure("exec_identity_changed");
      }
      auto result = syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
      close(fd);
      if (result && errno != ESRCH)
        throw runtime::Failure("exec_close_failed");
      for (int i = 0; i < 100; ++i) {
        auto observed = runtime::docker_json("GET", path);
        if (observed["ContainerID"] != d["containerId"])
          throw runtime::Failure("exec_identity_changed");
        if (observed["Running"] == false)
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      throw runtime::Failure("exec_close_not_observed");
    }
  }
#endif
}
void stop(const Json &d, const std::string &g) {
  auto dir = std::filesystem::path(d["directory"].get<std::string>());
  if (!std::filesystem::exists(dir))
    return;
  auto stored = console::load(dir / "config.json");
  if (stored["id"] != d["id"] || stored["nonce"] != d["nonce"])
    throw runtime::Failure("session_ownership_conflict");
  auto unlink_sockets = [&] {
    for (auto name : {"control.sock", "attach.sock", "serial.sock", "qmp.sock"}) {
      auto path = dir / name;
      struct stat st{};
      if (lstat(path.c_str(), &st)) {
        if (errno == ENOENT)
          continue;
        throw runtime::Failure("session_socket_inspection_failed");
      }
      if (!S_ISSOCK(st.st_mode) || st.st_uid != 0 || unlink(path.c_str()))
        throw runtime::Failure("session_socket_cleanup_failed");
    }
  };
  cleanup_docker(d);
  auto listed =
      runtime::process({"/usr/bin/systemctl", "list-units", "--all", "--no-legend", d["unit"]});
  if (listed.code)
    throw runtime::Failure("session_supervisor_unavailable");
  if (listed.output.empty()) {
    unlink_sockets();
    return;
  }

  // Closing the Docker exec can make its recorder exit between the supervisor
  // observation and the control request. Wait for that exit instead of trying
  // to adopt an already-closing socket once and stranding recovery.
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    try {
      request(d, "close", g);
      closed = true;
    } catch (...) {
      auto r = runtime::process(
          {"/usr/bin/systemctl", "show", d["unit"], "--property=ActiveState", "--value"});
      if (r.code)
        throw runtime::Failure("session_supervisor_unavailable");
      if (r.output == "inactive\n" || r.output == "failed\n") {
        closed = true;
        break;
      }
      try {
        request(d, "adopt", g);
        request(d, "close", g);
        closed = true;
      } catch (...) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
  if (!closed)
    throw runtime::Failure("session_close_not_observed");
  listed =
      runtime::process({"/usr/bin/systemctl", "list-units", "--all", "--no-legend", d["unit"]});
  if (listed.code)
    throw runtime::Failure("session_supervisor_unavailable");
  if (listed.output.empty()) {
    unlink_sockets();
    return;
  }
  auto r = runtime::process({"/usr/bin/systemctl", "stop", d["unit"]});
  if (r.code) {
    auto remaining =
        runtime::process({"/usr/bin/systemctl", "list-units", "--all", "--no-legend", d["unit"]});
    if (remaining.code || !remaining.output.empty())
      throw runtime::Failure("session_stop_failed");
  }
  runtime::detail::reap_stopped_unit(d["unit"]);
  unlink_sockets();
}
Json docker_session(const Json &r, const Json &resource, const std::filesystem::path &root,
                    bool input) {
  auto id = resource["identity"]["id"].get<std::string>();
  auto c = runtime::docker_json("GET", "/v1.52/containers/" + id + "/json");
  if (c["Id"] != id ||
      c["Config"]["Labels"].value("graphlab.run", "") != r["id"].get<std::string>() ||
      c["State"]["Running"] != true)
    throw runtime::Failure("terminal_container_changed");
  auto d = descriptor(r, resource["logical"], root, "docker");
  d["containerId"] = id;
  d["recordInput"] = input;
  return d;
}
Json ssh_session(const Json &r, const Json &resource, const std::filesystem::path &root,
                 bool input) {
  auto workload = resource["configuration"]["workload"].get<std::string>();
  auto vm = r["artifacts"]["workloads"][workload]["vm"];
  if (!vm.contains("sshUser") || !vm.contains("knownHostsSha256"))
    throw runtime::Failure("guest_ssh_not_configured");
  auto known = root / "artifacts" / vm["knownHostsSha256"].get<std::string>().substr(7);
  auto key = root / "credentials" / (workload + ".key");
  struct stat st{};
  if (lstat(key.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077))
    throw runtime::Failure("ssh_key_requires_private_root_file");
  if (lstat(known.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022) ||
      capture::file_hash(known) != vm["knownHostsSha256"].get<std::string>())
    throw runtime::Failure("ssh_host_identity_unpinned");
  std::string address;
  for (const auto &a : r["topology"]["managementAttachments"])
    if (a["endpoint"].get<std::string>().starts_with(resource["logical"].get<std::string>() +
                                                     ":")) {
      address = a["address"].get<std::string>();
      address = address.substr(0, address.find('/'));
      break;
    }
  if (address.empty())
    throw runtime::Failure("guest_management_unavailable");
  auto d = descriptor(r, resource["logical"], root / "sessions", "ssh");
  d["recordInput"] = input;
  d["sshAddress"] = address;
  d["sshUser"] = vm["sshUser"];
  d["sshKey"] = key.string();
  d["knownHosts"] = known.string();
  return d;
}
void docker_attach(const Json &d, const std::string &generation) {
#ifdef __linux__
  SessionLock lock(std::filesystem::path(d["directory"].get<std::string>()));
  // The worker receives only this already-authorized exec stream, never the Docker socket.
  auto created = runtime::docker_json(
      "POST", "/v1.52/containers/" + d["containerId"].get<std::string>() + "/exec",
      {{"AttachStdin", true},
       {"AttachStdout", true},
       {"AttachStderr", true},
       {"Tty", true},
       {"Cmd", Json::array({"/bin/sh"})}});
  auto exec = created["Id"].get<std::string>();
  runtime::detail::checkpoint("terminal.exec-created");
  auto dir = std::filesystem::path(d["directory"].get<std::string>());
  capture::atomic_json(dir / "exec.json", {{"id", exec}, {"containerId", d["containerId"]}});
  runtime::detail::checkpoint("terminal.exec-intent");
  int fd = connect_unix("/var/run/docker.sock");
  auto body = Json{{"Detach", false}, {"Tty", true}}.dump();
  auto wire = "POST /v1.52/exec/" + exec +
              "/start HTTP/1.1\r\nHost: docker\r\nConnection: Upgrade\r\nUpgrade: "
              "tcp\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(body.size()) + "\r\n\r\n" + body;
  if (send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(wire.size())) {
    close(fd);
    throw runtime::Failure("exec_start_failed");
  }
  std::string header;
  char ch;
  while (!header.ends_with("\r\n\r\n") && header.size() < 8192) {
    if (recv(fd, &ch, 1, 0) != 1) {
      close(fd);
      throw runtime::Failure("exec_upgrade_failed");
    }
    header += ch;
  }
  if (!header.starts_with("HTTP/1.1 101") && !header.starts_with("HTTP/1.1 200")) {
    close(fd);
    throw runtime::Failure("exec_upgrade_failed");
  }
  runtime::detail::checkpoint("terminal.created");
  int peer = -1;
  for (int i = 0; i < 100; ++i) {
    try {
      peer = connect_unix(dir / "attach.sock");
      break;
    } catch (...) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  if (peer < 0) {
    close(fd);
    throw runtime::Failure("terminal_attach_unavailable");
  }
  auto auth = Json{{"nonce", d["nonce"]}, {"generation", generation}}.dump();
  iovec io{auth.data(), auth.size()};
  char ancillary[CMSG_SPACE(sizeof(int))]{};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = ancillary;
  msg.msg_controllen = sizeof(ancillary);
  auto cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
  auto n = sendmsg(peer, &msg, MSG_NOSIGNAL);
  close(fd);
  close(peer);
  if (n != static_cast<ssize_t>(auth.size()))
    throw runtime::Failure("terminal_attach_failed");
#else
  (void)d;
  (void)generation;
  throw runtime::Failure("terminal_requires_linux");
#endif
}
void docker_resize(const Json &d, unsigned rows, unsigned columns) {
  if (rows < 1 || rows > 300 || columns < 1 || columns > 500)
    throw runtime::Failure("invalid_terminal_size");
  auto e = console::load(std::filesystem::path(d["directory"].get<std::string>()) / "exec.json");
  auto info = runtime::docker_json("GET", "/v1.52/exec/" + e["id"].get<std::string>() + "/json");
  if (info["ContainerID"] != d["containerId"] || info["Running"] != true)
    throw runtime::Failure("exec_identity_changed");
  runtime::docker_json("POST", "/v1.52/exec/" + e["id"].get<std::string>() + "/resize?h=" +
                                   std::to_string(rows) + "&w=" + std::to_string(columns));
}
namespace {
Json recordings(const Json &run) {
  auto list = run.value("sessions", Json::array());
  for (const auto &r : run["resources"])
    if (r["kind"] == "qemu" && r.contains("identity"))
      list.push_back(r["identity"]);
  return list;
}
} // namespace
Json artifacts(const Json &run) {
  Json list = Json::array();
  for (const auto &d : recordings(run)) {
    auto dir = std::filesystem::path(d["directory"].get<std::string>());
    try {
      auto m = console::load(dir / "manifest.json");
      Json a = {{"captureId", d["id"]},
                {"edge", d["node"]},
                {"mediaType", "application/x-graphlab-terminal"},
                {"recordInput", d["recordInput"]},
                {"state", m.value("state", "incomplete")}};
      if (m["state"] == "closed" && std::filesystem::is_regular_file(dir / "output.glterm")) {
        a["id"] = "terminal-" + d["id"].get<std::string>();
        a["sha256"] = m.at("sha256");
        a["size"] = std::to_string(std::filesystem::file_size(dir / "output.glterm"));
      } else
        a["state"] = "partial";
      list.push_back(a);
    } catch (...) {
      list.push_back({{"captureId", d["id"]},
                      {"edge", d["node"]},
                      {"state", "unavailable-or-partial"},
                      {"mediaType", "application/x-graphlab-terminal"}});
    }
  }
  return list;
}
Json download(const Json &run, const std::string &id, std::uint64_t offset) {
  for (const auto &d : recordings(run))
    if (id == "terminal-" + d["id"].get<std::string>()) {
      auto dir = std::filesystem::path(d["directory"].get<std::string>());
      auto m = console::load(dir / "manifest.json");
      if (m["state"] != "closed")
        throw runtime::Failure("recording_not_closed", 409);
      int fd = open((dir / "output.glterm").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0)
        throw runtime::Failure("recording_unavailable", 404);
      struct Guard {
        int fd;
        ~Guard() { close(fd); }
      } guard{fd};
      struct stat s{};
      if (fstat(fd, &s) || !S_ISREG(s.st_mode) || offset > std::uint64_t(s.st_size))
        throw runtime::Failure("invalid_recording_offset");
      char b[65536];
      auto n = pread(fd, b, sizeof(b), offset);
      if (n < 0)
        throw runtime::Failure("recording_read_failed");
      return {{"base64", encode({b, static_cast<std::size_t>(n)})},
              {"offset", std::to_string(offset)},
              {"next", std::to_string(offset + n)},
              {"eof", offset + n == std::uint64_t(s.st_size)},
              {"sha256", m.at("sha256")},
              {"size", std::to_string(s.st_size)}};
    }
  throw runtime::Failure("recording_not_found", 404);
}
} // namespace graphlab::terminal
