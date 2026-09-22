#include <charconv>
#include <fcntl.h>
#include <graphlab/console.hpp>
#include <graphlab/runtime.hpp>
#include <iostream>
#include <sys/file.h>
#include <unistd.h>
int main(int argc, char **argv) {
  try {
    if ((argc != 7 && argc != 11) || std::string(argv[1]) != "--socket" ||
        std::string(argv[3]) != "--topologies" || std::string(argv[5]) != "--lock") {
      std::cerr << "Usage: lab-agent --socket PATH --topologies DIR --lock FILE [--state "
                   "PRIVATE_DIR --allow-uid UID]\n";
      return 1;
    }
    graphlab::console::Catalog catalog(argv[4], argv[6]);
    if (argc == 7) {
      graphlab::console::run_agent(argv[2], geteuid(), catalog);
      return 0;
    }
#ifndef __linux__
    throw std::runtime_error("M2 execution requires Linux");
#else
    if (std::string(argv[7]) != "--state" || std::string(argv[9]) != "--allow-uid" ||
        geteuid() != 0)
      throw std::runtime_error("M2 requires root agent and explicit state/peer configuration");
    uid_t uid;
    std::string number = argv[10];
    auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), uid);
    if (ec != std::errc{} || end != number.data() + number.size())
      throw std::runtime_error("invalid UID");
    int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
      throw std::runtime_error("host executor already running");
    struct Guard {
      int fd;
      ~Guard() { close(fd); }
    } host_lock{lock};
    graphlab::runtime::LinuxBackend backend;
    graphlab::runtime::Engine engine(argv[8], backend, catalog);
    graphlab::console::run_agent(argv[2], uid, catalog, [&](const auto &request, uid_t principal) {
      auto method = request.value("method", "");
      if (method == "capabilities") {
        auto result = catalog.dispatch(request);
        result["readOnly"] = false;
        result["execution"] = true;
        result["runtimeMappings"] = true;
        result["developmentOnly"] = true;
        result["captureCoverage"] = "unavailable";
        return result;
      }
      if (method == "inventory")
        return engine.inventory(catalog.dispatch(request));
      if (method == "topologies" || method == "diagnostics")
        return catalog.dispatch(request);
      return engine.dispatch(request, principal);
    });
#endif
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
