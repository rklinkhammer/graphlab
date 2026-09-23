// PID 1 for a narrowly scoped QEMU container. No Docker socket or shell is used.
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <graphlab/qemu.hpp>
#include <iostream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
long long monotonic() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3 || std::string(argv[1]) != "--config")
    return 2;
  pid_t child = -1;
  try {
    struct stat st{};
    if (lstat(argv[2], &st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 0022) ||
        st.st_size > 65536)
      throw std::runtime_error("unsafe runner configuration");
    auto loaded = lab_support::read_document(argv[2]);
    if (!loaded)
      throw std::runtime_error("invalid runner configuration");
    auto c = lab_support::Json::parse(*loaded);
    if (c.at("kind") != "qemu" || !c.contains("runnerImage"))
      throw std::runtime_error("runner requires QEMU container descriptor");
    auto args = graphlab::qemu::arguments(c);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    auto started = monotonic();
    auto parent = getpid();
    child = fork();
    if (child < 0)
      throw std::runtime_error("fork failed");
    if (!child) {
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent)
        _exit(127);
      signal(SIGTERM, SIG_DFL);
      signal(SIGINT, SIG_DFL);
      std::vector<char *> av;
      for (auto &s : args)
        av.push_back(s.data());
      av.push_back(nullptr);
      execv(av[0], av.data());
      _exit(127);
    }
    while (!stopping) {
      int status;
      if (waitpid(child, &status, WNOHANG) == child) {
        child = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
      }
      long long heartbeat = 0;
      std::ifstream(std::filesystem::path(c.at("directory").get<std::string>()) /
                    "runner-heartbeat") >>
          heartbeat;
      auto now = monotonic();
      if ((heartbeat > 0 && (heartbeat > now || now - heartbeat > 5000000000LL)) ||
          (heartbeat == 0 && now - started > 10000000000LL))
        throw std::runtime_error("terminal watchdog lost: QEMU terminated");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    return 0;
  } catch (const std::exception &e) {
    if (child > 0) {
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
    }
    std::cerr << e.what() << '\n';
    return 1;
  }
}
