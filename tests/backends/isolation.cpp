// Demonstrate why a network namespace alone is not a separate OVS failure domain.
#include <fcntl.h>
#include <graphlab/runtime.hpp>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab;
int main() {
  if (geteuid())
    return 77;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  auto token = console::random_hex(4);
  std::vector<std::string> names;
  try {
    auto cmd = [](std::vector<std::string> args) {
      auto r = runtime::process(args);
      if (r.code)
        throw std::runtime_error("probe command failed");
      return r.output;
    };
    auto host = cmd({"/usr/bin/ovs-vsctl", "--format=json", "list", "Open_vSwitch"});
    ino_t prior = 0;
    for (int i = 0; i < 2; ++i) {
      auto name = "gl7-isolation-" + token + "-" + std::to_string(i);
      cmd({"/usr/sbin/ip", "netns", "add", name});
      names.push_back(name);
      struct stat st{};
      if (stat(("/run/netns/" + name).c_str(), &st) || st.st_ino == prior)
        throw std::runtime_error("namespace identity unavailable");
      prior = st.st_ino;
      auto observed = cmd({"/usr/sbin/ip", "netns", "exec", name, "/usr/bin/ovs-vsctl",
                           "--format=json", "list", "Open_vSwitch"});
      if (observed != host)
        throw std::runtime_error("unexpected OVS ownership: review before exposure");
      std::cout << "namespace=" << name << " inode=" << st.st_ino
                << " observes same host OVS database\n";
    }
    std::cout << "REJECT namespace-ovs: distinct network namespaces do not isolate the default OVS "
                 "database/daemon; no independent datapath/failure boundary qualified\n";
    for (const auto &n : names)
      cmd({"/usr/sbin/ip", "netns", "delete", n});
    std::cout << "PASS scoped namespace cleanup; shared daemon was not restarted\n";
  } catch (const std::exception &e) {
    for (const auto &n : names)
      runtime::process({"/usr/sbin/ip", "netns", "delete", n});
    std::cerr << e.what() << '\n';
    return 1;
  }
}
