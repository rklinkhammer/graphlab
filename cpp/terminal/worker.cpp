#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <graphlab/qemu.hpp>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
namespace {
volatile sig_atomic_t finishing = 0;
void finish(int) { finishing = 1; }
int listener(const std::filesystem::path &p) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  auto s = p.string();
  if (s.size() >= sizeof(a.sun_path))
    throw runtime::Failure("socket_path_limit");
  std::strcpy(a.sun_path, s.c_str());
  if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) || listen(fd, 8))
    throw runtime::Failure("terminal_bind_failed");
  return fd;
}
bool root_peer(int fd) {
  ucred p{};
  socklen_t size = sizeof(p);
  return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &p, &size) && p.uid == 0;
}
std::string boot() {
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  std::string s;
  f >> s;
  return s;
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--cleanup" && geteuid() == 0) {
    try {
      struct stat st{};
      if (lstat(argv[2], &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077))
        return 2;
      auto config = console::load(argv[2]);
      if (config["bootId"] != boot())
        return 2;
      terminal::cleanup_docker(config);
      return 0;
    } catch (...) {
      return 1;
    }
  }
  if (argc != 3 || std::string(argv[1]) != "--config" || geteuid() != 0)
    return 2;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, finish);
  signal(SIGINT, finish);
  umask(0077);
  auto dir = std::filesystem::path(argv[2]).parent_path();
  Json state;
  std::unique_ptr<terminal::Recording> recording;
  int source = -1, server = -1, attach = -1;
  pid_t child = -1;
  bool fresh = false;
  auto persist = [&] {
    state["heartbeatAt"] = console::timestamp();
    if (recording) {
      state["outputOffset"] = std::to_string(recording->offset());
      state["sequence"] = std::to_string(recording->sequence());
    }
    capture::atomic_json(dir / "manifest.json", state);
  };
  auto killchild = [&] {
    if (child > 0) {
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      child = -1;
    }
  };
  try {
    auto c = console::load(argv[2]);
    if (c["bootId"] != boot())
      throw runtime::Failure("terminal_prior_boot");
    int marker =
        open((dir / "worker.started").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (marker < 0)
      throw runtime::Failure("terminal_identity_reused");
    fsync(marker);
    close(marker);
    fresh = true;
    auto invocation = getenv("INVOCATION_ID");
    if (!invocation)
      throw runtime::Failure("terminal_requires_independent_unit");
    state = {{"apiVersion", "graphlab.session/v1"},
             {"id", c["id"]},
             {"runId", c["runId"]},
             {"node", c["node"]},
             {"kind", c["kind"]},
             {"bootId", boot()},
             {"generation", c["generation"]},
             {"invocationId", invocation},
             {"state", "starting"},
             {"createdAt", c["createdAt"]},
             {"recordInput", c["recordInput"]},
             {"serialCoverage", "from-attachment"},
             {"guestReady", false}};
    recording = std::make_unique<terminal::Recording>(dir / "output.partial", c["byteBudget"]);
    recording->append(3, Json{{"event", "created"},
                              {"wallClock", console::timestamp()},
                              {"bootId", boot()},
                              {"recordInput", c["recordInput"]}}
                             .dump());
    server = listener(dir / "control.sock");
    bool vm = c["kind"] == "qemu";
    bool leased = false;
    auto deadline = terminal::Clock::time_point::min();
    if (vm) {
      if (!c.contains("runnerImage")) {
        auto args = qemu::arguments(c);
        child = fork();
        if (child < 0)
          throw runtime::Failure("qemu_fork_failed");
        if (child == 0) {
          prctl(PR_SET_PDEATHSIG, SIGKILL);
          if (getppid() == 1)
            _exit(127);
          int log = open((dir / "qemu.log").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
          dup2(log, 1);
          dup2(log, 2);
          int input = open("/dev/null", O_RDONLY);
          dup2(input, 0);
          std::vector<char *> av;
          for (auto &s : args)
            av.push_back(s.data());
          av.push_back(nullptr);
          execv(av[0], av.data());
          _exit(127);
        }
      }
      for (int i = 0; i < 100; ++i) {
        try {
          auto status = qemu::qmp(dir / "qmp.sock", "query-status");
          if (status["running"] == false) {
            source = terminal::connect_unix(dir / "serial.sock");
            break;
          }
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (source < 0)
        throw runtime::Failure("qemu_serial_not_ready");
      state["serialReady"] = true;
      state["vmState"] = "paused";
      if (c.contains("runnerImage"))
        state["runnerId"] = c.at("runnerId");
      else
        state["qemuPid"] = child;
      state["qemuVersion"] = qemu::qmp(dir / "qmp.sock", "query-version");
    } else if (c["kind"] == "ssh") {
      winsize size{24, 80, 0, 0};
      child = forkpty(&source, nullptr, nullptr, &size);
      if (child < 0)
        throw runtime::Failure("ssh_pty_failed");
      if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1)
          _exit(127);
        std::vector<std::string> args = {"/usr/bin/ssh",
                                         "-F",
                                         "/dev/null",
                                         "-tt",
                                         "-oBatchMode=yes",
                                         "-oConnectTimeout=10",
                                         "-oConnectionAttempts=5",
                                         "-oStrictHostKeyChecking=yes",
                                         "-oGlobalKnownHostsFile=/dev/null",
                                         "-oUserKnownHostsFile=" +
                                             c["knownHosts"].get<std::string>(),
                                         "-oIdentitiesOnly=yes",
                                         "-i",
                                         c["sshKey"],
                                         "-l",
                                         c["sshUser"],
                                         c["sshAddress"]};
        std::vector<char *> av;
        for (auto &s : args)
          av.push_back(s.data());
        av.push_back(nullptr);
        execv(av[0], av.data());
        _exit(127);
      }
    } else if (c["kind"] == "docker")
      attach = listener(dir / "attach.sock");
    else
      throw runtime::Failure("unsupported_terminal_source");
    if (source >= 0)
      fcntl(source, F_SETFL, O_NONBLOCK);
    state["state"] = "active";
    recording->sync();
    persist();
    terminal::Lease writer;
    auto last = terminal::Clock::now(), activity = last;
    std::string readiness;
    auto pause = [&] {
      qemu::qmp(dir / "qmp.sock", "stop");
      auto s = qemu::qmp(dir / "qmp.sock", "query-status");
      if (s["running"] != false)
        throw runtime::Failure("vm_pause_not_acknowledged");
      leased = false;
      state["vmState"] = "paused";
      recording->append(3, "paused");
    };
    auto heartbeat_at = terminal::Clock::time_point::min();
    while (!finishing) {
      auto now = terminal::Clock::now();
      if (vm && c.contains("runnerImage") &&
          (heartbeat_at == terminal::Clock::time_point::min() ||
           now - heartbeat_at > std::chrono::seconds(1))) {
        auto path = dir / "runner-heartbeat.next";
        std::ofstream out(path);
        out << std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        out.close();
        if (!out)
          throw runtime::Failure("runner_heartbeat_failed");
        std::filesystem::rename(path, dir / "runner-heartbeat");
        heartbeat_at = now;
      }
      if (vm && leased && now >= deadline) {
        pause();
        state["leaseExpiredAt"] = console::timestamp();
        persist();
      }
      if (!vm && now - activity > std::chrono::minutes(30)) {
        finishing = 1;
        break;
      }
      if (child > 0) {
        int status;
        if (waitpid(child, &status, WNOHANG) == child) {
          child = -1;
          // Drain bytes already queued by a source that exited before this poll.
          if (source >= 0) {
            char tail[4096];
            for (;;) {
              auto n = read(source, tail, sizeof(tail));
              if (n < 0 && errno == EINTR)
                continue;
              if (n <= 0)
                break;
              recording->append(1, {tail, static_cast<std::size_t>(n)});
            }
          }
          if (!vm && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            finishing = 1;
            break;
          }
          throw runtime::Failure(vm ? "qemu_exited" : "terminal_source_exited");
        }
      }
      pollfd fds[3] = {{server, POLLIN, 0}, {source, POLLIN, 0}, {attach, POLLIN, 0}};
      poll(fds, 3, 25);
      if (source >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
        char b[4096];
        auto n = read(source, b, sizeof(b));
        if (n > 0) {
          recording->append(1, {b, static_cast<std::size_t>(n)});
          readiness.append(b, n);
          if (readiness.find("GRAPHLAB_READY") != std::string::npos)
            state["guestReady"] = true;
          if (readiness.size() > 256)
            readiness.erase(0, readiness.size() - 256);
        } else if (n == 0) {
          if (vm && c.contains("runnerImage"))
            throw runtime::Failure("qemu_runner_exited");
          finishing = 1;
          break;
        }
      }
      if (attach >= 0 && (fds[2].revents & POLLIN)) {
        int peer = accept4(attach, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (peer >= 0) {
          if (root_peer(peer)) {
            char b[1024], ancillary[CMSG_SPACE(sizeof(int))]{};
            iovec io{b, sizeof(b)};
            msghdr m{};
            m.msg_iov = &io;
            m.msg_iovlen = 1;
            m.msg_control = ancillary;
            m.msg_controllen = sizeof(ancillary);
            pollfd ready{peer, POLLIN, 0};
            poll(&ready, 1, 100);
            auto n = recvmsg(peer, &m, MSG_CMSG_CLOEXEC);
            auto cm = CMSG_FIRSTHDR(&m);
            if (n > 0 && cm && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
              int received;
              std::memcpy(&received, CMSG_DATA(cm), sizeof(received));
              try {
                auto auth = Json::parse(std::string(b, n));
                if (auth["nonce"] != c["nonce"] || auth["generation"] != state["generation"] ||
                    source >= 0)
                  throw runtime::Failure("attach_denied");
                source = received;
                fcntl(source, F_SETFL, O_NONBLOCK);
                state["sourceReady"] = true;
                persist();
              } catch (...) {
                close(received);
              }
            }
          }
          close(peer);
        }
      }
      if (fds[0].revents & POLLIN) {
        int peer = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0)
          continue;
        timeval t{0, 100000};
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
        Json response;
        try {
          if (!root_peer(peer))
            throw runtime::Failure("terminal_peer_denied");
          char b[8192];
          auto n = recv(peer, b, sizeof(b), 0);
          if (n <= 0)
            throw runtime::Failure("terminal_request_invalid");
          auto req = Json::parse(std::string(b, n));
          if (req["nonce"] != c["nonce"])
            throw runtime::Failure("terminal_auth_denied");
          auto op = req["operation"].get<std::string>();
          auto generation = req["generation"].get<std::string>();
          auto p = req.value("params", Json::object());
          if (op == "adopt") {
            if (std::stoull(generation) <= std::stoull(state["generation"].get<std::string>()))
              throw runtime::Failure("stale_controller");
            state["generation"] = generation;
            writer.revoke();
            if (vm)
              pause();
            persist();
          } else if (generation != state["generation"].get<std::string>())
            throw runtime::Failure("stale_controller");
          response = state;
          if (op == "close")
            finishing = 1;
          else if (op == "status" || op == "adopt") {
          } else if (op == "release" && vm) {
            deadline = terminal::Clock::now() + std::chrono::seconds(10);
            leased = true;
            qemu::qmp(dir / "qmp.sock", "cont");
            state["vmState"] = "running";
            recording->append(3, "continued");
            response = state;
          } else if (op == "renew" && vm) {
            if (!leased || terminal::Clock::now() >= deadline)
              throw runtime::Failure("vm_lease_expired");
            deadline = terminal::Clock::now() + std::chrono::seconds(10);
          } else if (op == "quiesce" && vm) {
            pause();
            response = state;
          } else if (op == "acquire") {
            response = writer.acquire(p.at("owner"), p.value("takeover", false));
          } else if (op == "renew-writer") {
            writer.renew(p.at("owner"), p.at("token"));
          } else if (op == "revoke") {
            writer.revoke();
          } else if (op == "replay") {
            recording->sync();
            response =
                terminal::replay(dir / "output.partial", std::stoull(p.value("sequence", "0")));
          } else if (op == "input" || op == "resize") {
            writer.require(p.at("owner"), p.at("token"));
            if (source < 0)
              throw runtime::Failure("terminal_source_unavailable");
            activity = terminal::Clock::now();
            if (op == "resize") {
              auto rows = p.at("rows").get<int>(), cols = p.at("columns").get<int>();
              if (rows < 1 || rows > 300 || cols < 1 || cols > 500)
                throw runtime::Failure("invalid_terminal_size");
              if (c["kind"] == "ssh") {
                winsize size{static_cast<unsigned short>(rows), static_cast<unsigned short>(cols),
                             0, 0};
                if (ioctl(source, TIOCSWINSZ, &size))
                  throw runtime::Failure("pty_resize_failed");
              }
              recording->append(2, Json{{"rows", rows}, {"columns", cols}}.dump());
            } else {
              auto bytes = terminal::decode(p.at("base64"));
              if (bytes.size() > 1024)
                throw runtime::Failure("terminal_input_limit");
              if (c["recordInput"] == true)
                recording->append(4, bytes);
              else
                recording->append(3, Json{{"event", "input"}, {"bytes", bytes.size()}}.dump());
              auto n = c["kind"] == "ssh" ? write(source, bytes.data(), bytes.size())
                                          : send(source, bytes.data(), bytes.size(), MSG_NOSIGNAL);
              if (n != static_cast<ssize_t>(bytes.size()))
                throw runtime::Failure("terminal_input_backpressure");
            }
          } else
            throw runtime::Failure("unsupported_terminal_operation");
        } catch (const std::exception &e) {
          response = {{"error", e.what()}};
        }
        auto b = response.dump();
        send(peer, b.data(), b.size(), MSG_NOSIGNAL);
        close(peer);
      }
      if (terminal::Clock::now() - last >= std::chrono::seconds(1)) {
        recording->sync();
        persist();
        last = terminal::Clock::now();
      }
    }
    if (vm && child > 0)
      pause();
    if (source >= 0) {
      close(source);
      source = -1;
    }
    killchild();
    recording->append(3, "closed");
    recording->close();
    state["state"] = "closed";
    state["sha256"] = capture::file_hash(dir / "output.glterm");
    persist();
    return 0;
  } catch (const std::exception &e) {
    killchild();
    if (source >= 0)
      close(source);
    if (fresh) {
      state["state"] = "incomplete";
      state["error"] = e.what();
      try {
        persist();
      } catch (...) {
      }
    }
    return 1;
  }
}
