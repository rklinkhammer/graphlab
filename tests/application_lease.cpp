#include "../packages/lab-support/cpp/record_io.hpp"
#include <array>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
using namespace lab_support::detail;
using namespace std::chrono_literals;
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
  std::cout << "PASS " << why << '\n';
}
struct Pair {
  int fd[2];
  Pair() {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd))
      throw std::runtime_error("socketpair");
    for (auto f : fd)
      if (fcntl(f, F_SETFL, O_NONBLOCK))
        throw std::runtime_error("nonblocking");
  }
  ~Pair() {
    for (auto f : fd)
      close(f);
  }
};
int main() {
  try {
    char record[16];
    std::fill_n(record, 16, 'A');
    {
      Pair p;
      auto deadline = Clock::now() + 1s;
      check(record_io(p.fd[0], record, true, deadline), "authorized write succeeds");
      char received[16];
      check(record_io(p.fd[1], received, false, deadline) &&
                std::equal(record, record + 16, received),
            "authorized record survives intact");
      check(!record_io(p.fd[0], record, true, Clock::now() - 1ms),
            "expired writable socket cannot send");
      check(recv(p.fd[1], received, 16, 0) < 0 && errno == EAGAIN,
            "expired write emits no payload");
    }
    for (bool partial : {false, true}) {
      Pair p;
      auto deadline = Clock::now() + 40ms;
      if (partial)
        check(send(p.fd[1], record, 8, 0) == 8, "partial prefix supplied");
      std::jthread writer([&] {
        std::this_thread::sleep_for(100ms);
        send(p.fd[1], record, partial ? 8 : 16, 0);
      });
      char received[16];
      bool read = record_io(p.fd[0], received, false, deadline);
      bool echoed = read && record_io(p.fd[0], received, true, deadline);
      writer.join();
      check(!read && !echoed, partial ? "partial record crossing lease cannot echo"
                                      : "delayed record crossing lease cannot echo");
      check(recv(p.fd[1], received, 16, 0) < 0 && errno == EAGAIN, "no echo after delayed input");
    }
    {
      Pair p;
      std::array<char, 4096> filler{};
      std::size_t queued = 0;
      for (;;) {
        auto n = send(p.fd[0], filler.data(), filler.size(), 0);
        if (n > 0)
          queued += n;
        else
          break;
      }
      check(errno == EAGAIN || errno == EWOULDBLOCK, "send buffer saturated");
      std::jthread reader([&] {
        std::this_thread::sleep_for(100ms);
        std::size_t drained = 0;
        while (drained < queued) {
          auto n = recv(p.fd[1], filler.data(), filler.size(), 0);
          if (n > 0)
            drained += n;
          else
            std::this_thread::yield();
        }
      });
      check(!record_io(p.fd[0], record, true, Clock::now() + 40ms),
            "blocked write stops at traffic deadline");
      reader.join();
      char received[16];
      check(recv(p.fd[1], received, 16, 0) < 0 && errno == EAGAIN,
            "readiness restored after expiry cannot leak record");
      check(!wait_ready(p.fd[0], POLLOUT, Clock::now() - 1ms),
            "expired connect/readiness wait rejects ready descriptor");
    }
    {
      Pair p;
      char received[16];
      auto began = Clock::now();
      check(!record_io(p.fd[0], received, false, Deadline::max()),
            "ordinary I/O timeout retained without traffic lease");
      check(Clock::now() - began < 2s, "unleased read remains bounded");
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
