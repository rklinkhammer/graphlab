#include <array>
#include <csignal>
#include <cstring>
#include <graphlab/console.hpp>
#include <graphlab/runtime.hpp>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace graphlab::console {
using Socket = asio::local::stream_protocol::socket;
uid_t peer_uid(int fd) {
#ifdef __linux__
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size))
    throw std::runtime_error("peer_credentials_unavailable");
  return credentials.uid;
#else
  uid_t uid;
  gid_t gid;
  if (getpeereid(fd, &uid, &gid))
    throw std::runtime_error("peer_credentials_unavailable");
  return uid;
#endif
}
void require_peer(int fd, uid_t expected) {
  if (peer_uid(fd) != expected)
    throw std::runtime_error("peer_denied");
}
namespace {
std::uint32_t frame_size(const std::array<unsigned char, 4> &h) {
  return (std::uint32_t(h[0]) << 24) | (std::uint32_t(h[1]) << 16) | (std::uint32_t(h[2]) << 8) |
         h[3];
}
std::string frame(const Json &j) {
  auto body = j.dump();
  if (body.size() > max_frame)
    throw std::runtime_error("frame_limit");
  std::string result(4, '\0');
  for (int i = 0; i < 4; ++i)
    result[i] = static_cast<char>(body.size() >> (24 - 8 * i));
  return result + body;
}
struct Connection : std::enable_shared_from_this<Connection> {
  Socket socket;
  asio::steady_timer timer;
  const Catalog &catalog;
  std::shared_ptr<std::size_t> count;
  std::array<unsigned char, 4> header{};
  std::string body, output;
  std::function<Json(const Json &, uid_t)> control;
  uid_t principal;
  Connection(Socket s, const Catalog &c, std::shared_ptr<std::size_t> n,
             std::function<Json(const Json &, uid_t)> handler, uid_t uid)
      : socket(std::move(s)), timer(socket.get_executor()), catalog(c), count(std::move(n)),
        control(std::move(handler)), principal(uid) {
    ++*count;
  }
  ~Connection() { --*count; }
  void close() {
    boost::system::error_code ec;
    socket.close(ec);
    timer.cancel();
  }
  void start() {
    auto self = shared_from_this();
    timer.expires_after(std::chrono::seconds(3));
    timer.async_wait([self](auto ec) {
      if (!ec)
        self->close();
    });
    asio::async_read(socket, asio::buffer(header), [self](auto ec, auto) {
      auto n = frame_size(self->header);
      if (ec || n == 0 || n > 4096)
        return self->close();
      self->body.resize(n);
      asio::async_read(self->socket, asio::buffer(self->body), [self](auto ec, auto) {
        if (ec)
          return self->close();
        Json response;
        try {
          auto request = lab_support::parse_document(self->body);
          if (!request || !Json::accept(self->body))
            throw std::runtime_error("invalid_request");
          response = {{"apiVersion", "graphlab.rpc/v1"},
                      {"ok", true},
                      {"result", self->control ? self->control(*request, self->principal)
                                               : self->catalog.dispatch(*request)}};
        } catch (const runtime::Failure &e) {
          response = {{"apiVersion", "graphlab.rpc/v1"},
                      {"ok", false},
                      {"error", e.what()},
                      {"status", e.status}};
        } catch (const std::exception &) {
          response = {
              {"apiVersion", "graphlab.rpc/v1"}, {"ok", false}, {"error", "request_rejected"}};
        }
        self->output = frame(response);
        asio::async_write(self->socket, asio::buffer(self->output),
                          [self](auto, auto) { self->close(); });
      });
    });
  }
};
} // namespace
Json rpc(const std::string &path, uid_t expected, const std::string &method, const Json &params) {
  asio::io_context io;
  Socket socket(io);
  asio::steady_timer timer(io);
  std::string output = frame(
                  {{"apiVersion", "graphlab.rpc/v1"}, {"method", method}, {"params", params}}),
              input;
  std::array<unsigned char, 4> header{};
  bool success = false;
  auto finish = [&] {
    boost::system::error_code ec;
    socket.close(ec);
    timer.cancel();
  };
  timer.expires_after(std::chrono::seconds(3));
  timer.async_wait([&](auto ec) {
    if (!ec)
      finish();
  });
  socket.async_connect(asio::local::stream_protocol::endpoint(path), [&](auto ec) {
    if (ec)
      return finish();
    try {
      require_peer(socket.native_handle(), expected);
    } catch (...) {
      return finish();
    }
    asio::async_write(socket, asio::buffer(output), [&](auto ec, auto) {
      if (ec)
        return finish();
      asio::async_read(socket, asio::buffer(header), [&](auto ec, auto) {
        auto n = frame_size(header);
        if (ec || n == 0 || n > max_frame)
          return finish();
        input.resize(n);
        asio::async_read(socket, asio::buffer(input), [&](auto ec, auto) {
          success = !ec;
          finish();
        });
      });
    });
  });
  io.run();
  if (!success)
    throw std::runtime_error("agent_unavailable");
  auto response = Json::parse(input);
  if (response.value("apiVersion", "") == "graphlab.rpc/v1" &&
      response.value("ok", true) == false && response.contains("status"))
    throw runtime::Failure(response.value("error", "agent_rejected"), response["status"]);
  if (response.value("apiVersion", "") != "graphlab.rpc/v1" || !response.value("ok", false) ||
      !response.contains("result"))
    throw std::runtime_error("agent_rejected");
  return response["result"];
}
void run_agent(const std::string &path, uid_t allowed, const Catalog &catalog,
               std::function<Json(const Json &, uid_t)> control) {
  auto parent = std::filesystem::path(path).parent_path();
  struct stat metadata{};
  if (lstat(parent.c_str(), &metadata) || !S_ISDIR(metadata.st_mode) ||
      metadata.st_uid != geteuid() || (metadata.st_mode & (allowed == geteuid() ? 0077 : 0027)))
    throw std::runtime_error("socket parent must be an owned private directory (0700)");
  if (std::filesystem::exists(path))
    throw std::runtime_error("socket path already exists; refusing to replace it");
  asio::io_context io;
  asio::local::stream_protocol::acceptor acceptor(io);
  acceptor.open();
  auto mask = umask(0077);
  boost::system::error_code bind_error;
  acceptor.bind(asio::local::stream_protocol::endpoint(path), bind_error);
  umask(mask);
  if (bind_error)
    throw boost::system::system_error(bind_error);
  struct stat bound{};
  if (lstat(path.c_str(), &bound))
    throw std::runtime_error("socket_identity_unavailable");
  struct Cleanup {
    std::string path;
    dev_t device;
    ino_t inode;
    ~Cleanup() {
      struct stat current{};
      if (!lstat(path.c_str(), &current) && S_ISSOCK(current.st_mode) && current.st_dev == device &&
          current.st_ino == inode)
        ::unlink(path.c_str());
    }
  } cleanup{path, bound.st_dev, bound.st_ino};
  if (allowed != geteuid()) {
    auto account = getpwuid(allowed);
    if (!account || metadata.st_gid != account->pw_gid ||
        chown(path.c_str(), geteuid(), account->pw_gid) || chmod(path.c_str(), 0660))
      throw std::runtime_error("socket_group_setup_failed");
  }
  acceptor.listen(32);
  auto active = std::make_shared<std::size_t>(0);
  std::function<void()> accept;
  accept = [&] {
    acceptor.async_accept([&](auto ec, Socket socket) {
      if (!ec) {
        try {
          require_peer(socket.native_handle(), allowed);
          if (*active < 32)
            std::make_shared<Connection>(std::move(socket), catalog, active, control, allowed)
                ->start();
        } catch (...) { /* Reject before reading caller data. */
        }
      }
      if (acceptor.is_open())
        accept();
    });
  };
  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](auto, auto) { io.stop(); });
  accept();
  io.run();
}
} // namespace graphlab::console
