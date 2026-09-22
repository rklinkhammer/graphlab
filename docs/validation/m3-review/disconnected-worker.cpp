#include <csignal>
#include <cstring>
#include <graphlab/capture.hpp>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
int main() {
  char pattern[] = "/tmp/m3-pipe-XXXXXX";
  std::filesystem::path dir = mkdtemp(pattern);
  auto path = (dir / "control.sock").string();
  int server = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strcpy(a.sun_path, path.c_str());
  bind(server, (sockaddr *)&a, sizeof(a));
  listen(server, 8);
  pid_t child = fork();
  if (!child) {
    signal(SIGPIPE, SIG_DFL);
    close(server);
    for (int i = 0; i < 10000; i++)
      try {
        graphlab::capture::worker_call(
            {{"directory", dir.string()}, {"nonce", std::string(64, 'a')}}, "status", "1");
      } catch (...) {
      }
    _exit(0);
  }
  int status = 0;
  for (;;) {
    if (waitpid(child, &status, WNOHANG) == child)
      break;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(server, &fds);
    timeval t{0, 10000};
    if (select(server + 1, &fds, nullptr, nullptr, &t) > 0) {
      int peer = accept(server, nullptr, nullptr);
      shutdown(peer, SHUT_RDWR);
      close(peer);
    }
  }
  close(server);
  std::filesystem::remove_all(dir);
  std::cout << "caller_signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << "\n";
  return WIFSIGNALED(status) ? 1 : 0;
}
