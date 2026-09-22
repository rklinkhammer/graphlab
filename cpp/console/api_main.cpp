#include <charconv>
#include <graphlab/console.hpp>
#include <iostream>
#include <unistd.h>
int main(int argc, char **argv) {
  try {
    if (geteuid() == 0)
      throw std::runtime_error("lab-api must run as an unprivileged user");
    if (argc == 3 && std::string(argv[1]) == "init-auth") {
      graphlab::console::initialize_auth(argv[2]);
      return 0;
    }
    if ((argc != 9 && argc != 11) || std::string(argv[1]) != "--socket" ||
        std::string(argv[3]) != "--auth" || std::string(argv[5]) != "--assets" ||
        std::string(argv[7]) != "--port") {
      std::cerr << "Usage: lab-api init-auth FILE\n       lab-api --socket PATH --auth FILE "
                   "--assets console/web/dist --port 8088 [--agent-uid UID]\n";
      return 1;
    }
    unsigned short port = 0;
    auto value = std::string(argv[8]);
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (ec != std::errc{} || end != value.data() + value.size() || port < 1024)
      throw std::runtime_error("port must be 1024..65535");
    uid_t agent_uid = geteuid();
    if (argc == 11) {
      if (std::string(argv[9]) != "--agent-uid")
        throw std::runtime_error("expected --agent-uid");
      std::string uid = argv[10];
      auto [last, result] = std::from_chars(uid.data(), uid.data() + uid.size(), agent_uid);
      if (result != std::errc{} || last != uid.data() + uid.size())
        throw std::runtime_error("invalid agent UID");
    }
    graphlab::console::Router router("127.0.0.1:" + value, argv[2], agent_uid, argv[4], argv[6]);
    graphlab::console::run_http(port, router);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
