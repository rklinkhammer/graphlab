// Shared C++ support for the disposable guest templates, also used as their serial/SSH console.
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
int main() {
  const bool init = getpid() == 1;
  int socket = -1;
  bool network = false;
  if (init) {
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, nullptr);
    mount("sysfs", "/sys", "sysfs", 0, nullptr);
    mount("devtmpfs", "/dev", "devtmpfs", 0, nullptr);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, "mode=620");
    signal(SIGCHLD, SIG_IGN);
    for (int pass = 0; pass < 5; ++pass)
      if (std::filesystem::exists("/modules"))
        for (const auto &e : std::filesystem::directory_iterator("/modules")) {
          int fd = open(e.path().c_str(), O_RDONLY);
          if (fd >= 0) {
            syscall(SYS_finit_module, fd, "", 0);
            close(fd);
          }
        }
    socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    for (int i = 0; i < 2; ++i) {
      ifreq r{};
      std::strcpy(r.ifr_name, i == 0 ? "eth0" : "eth1");
      if (ioctl(socket, SIOCGIFFLAGS, &r))
        continue;
      r.ifr_flags |= IFF_UP;
      if (ioctl(socket, SIOCSIFFLAGS, &r))
        continue;
      sockaddr_in ip{};
      ip.sin_family = AF_INET;
      inet_pton(AF_INET, i == 0 ? "10.233.17.2" : "172.31.243.10", &ip.sin_addr);
      std::memcpy(&r.ifr_addr, &ip, sizeof(ip));
      if (ioctl(socket, SIOCSIFADDR, &r))
        continue;
      inet_pton(AF_INET, "255.255.255.0", &ip.sin_addr);
      std::memcpy(&r.ifr_netmask, &ip, sizeof(ip));
      if (ioctl(socket, SIOCSIFNETMASK, &r))
        continue;
      if (i == 0)
        network = true;
    }
    if (access("/usr/sbin/dropbear", X_OK) == 0 && fork() == 0) {
      execl("/usr/sbin/dropbear", "dropbear", "-F", "-p", "172.31.243.10:22", "-r",
            "/etc/dropbear/dropbear_ed25519_host_key", nullptr);
      _exit(127);
    }
  }
  utsname u{};
  uname(&u);
  std::cout << '\n'
            << (init ? (network ? "GRAPHLAB_READY" : "GRAPHLAB_NETWORK_FAILED")
                     : "GRAPHLAB_CONSOLE")
            << " " << u.machine << " C++ guest fixture\nCommands: status, echo TEXT, exit, help\n> "
            << std::flush;
  auto next = std::chrono::steady_clock::now();
  std::string line;
  unsigned sequence = 0;
  for (;;) {
    pollfd p{0, POLLIN, 0};
    poll(&p, 1, 50);
    if (p.revents & POLLIN) {
      char b[128];
      auto n = read(0, b, sizeof(b));
      if (n == 0 && !init)
        return 0;
      for (int i = 0; i < n; ++i) {
        if (b[i] == '\r' || b[i] == '\n') {
          if (line == "status")
            std::cout << "guest=" << u.machine << " packets=" << sequence << '\n';
          else if (line.starts_with("echo "))
            std::cout << line.substr(5) << '\n';
          else if (line == "exit" && !init)
            return 0;
          else
            std::cout << "status | echo TEXT | exit | help\n";
          line.clear();
          std::cout << "> " << std::flush;
        } else if (line.size() < 1024)
          line += b[i];
      }
    }
    if (init && std::chrono::steady_clock::now() >= next) {
      auto b = "graphlab-guest-" + std::to_string(sequence++);
      sockaddr_in dest{};
      dest.sin_family = AF_INET;
      dest.sin_port = htons(49000);
      inet_pton(AF_INET, "10.233.17.1", &dest.sin_addr);
      sendto(socket, b.data(), b.size(), 0, reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
      next = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    }
  }
}
