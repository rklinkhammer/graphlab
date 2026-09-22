#include <boost/beast.hpp>
#include <fcntl.h>
#include <graphlab/runtime.hpp>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
namespace graphlab::runtime {
ProcessResult process(const std::vector<std::string> &args, int timeout_seconds) {
  if (args.empty() || args[0].empty() || args[0][0] != '/')
    throw Failure("invalid_process_path");
  std::vector<char *> argv;
  for (const auto &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  int fds[2];
  if (pipe(fds))
    throw Failure("process_pipe");
  [[maybe_unused]] auto parent = getpid();
  auto pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent)
      _exit(127);
#endif
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    throw Failure("process_fork");
  }
  setpgid(pid, pid);
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  std::string output;
  int status = 0;
  bool timeout = false, done = false;
  while (!done) {
    char buffer[4096];
    ssize_t n;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) {
      if (output.size() + n > 1024 * 1024) {
        timeout = true;
        break;
      }
      output.append(buffer, n);
    }
    if (waitpid(pid, &status, WNOHANG) == pid) {
      done = true;
      break;
    }
    if (timeout || std::chrono::steady_clock::now() > deadline) {
      timeout = true;
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
    pollfd p{fds[0], POLLIN, 0};
    poll(&p, 1, 10);
  }
  char buffer[4096];
  ssize_t n;
  while ((n = read(fds[0], buffer, sizeof(buffer))) > 0 && output.size() + n <= 1024 * 1024)
    output.append(buffer, n);
  close(fds[0]);
  if (timeout)
    throw Failure("process_deadline_or_output_limit", 503);
  return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, output};
}
DockerResponse docker_request(const std::string &method, const std::string &path,
                              const Json &body) {
  namespace asio = boost::asio;
  namespace http = boost::beast::http;
  asio::io_context io;
  asio::local::stream_protocol::socket socket(io);
  asio::steady_timer timer(io);
  http::request<http::string_body> request{http::string_to_verb(method), path, 11};
  request.set(http::field::host, "docker");
  request.set(http::field::content_type, "application/json");
  request.keep_alive(false);
  if (!body.is_null())
    request.body() = body.dump();
  request.prepare_payload();
  boost::beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  parser.body_limit(4 * 1024 * 1024);
  bool success = false;
  auto finish = [&] {
    boost::system::error_code ec;
    socket.close(ec);
    timer.cancel();
  };
  timer.expires_after(std::chrono::seconds(15));
  timer.async_wait([&](auto ec) {
    if (!ec)
      finish();
  });
  socket.async_connect(asio::local::stream_protocol::endpoint("/var/run/docker.sock"),
                       [&](auto ec) {
                         if (ec)
                           return finish();
                         http::async_write(socket, request, [&](auto ec, auto) {
                           if (ec)
                             return finish();
                           http::async_read(socket, buffer, parser, [&](auto ec, auto) {
                             success = !ec;
                             finish();
                           });
                         });
                       });
  io.run();
  if (!success)
    throw Failure("docker_unavailable", 503);
  return {parser.get().result_int(), parser.get().body()};
}
Json docker_json(const std::string &method, const std::string &path, const Json &body) {
  auto response = docker_request(method, path, body);
  if (response.status < 200 || response.status >= 300)
    throw Failure("docker_request_failed_" + std::to_string(response.status), 503);
  return response.body.empty() ? Json::object() : Json::parse(response.body);
}
} // namespace graphlab::runtime
