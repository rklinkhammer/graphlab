#include <boost/beast.hpp>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <graphlab/console.hpp>
#include <graphlab/runtime.hpp>
#include <graphlab/terminal.hpp>
#include <iostream>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>

namespace graphlab::console {
namespace {
std::string hex(const unsigned char *data, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0; i < size; ++i) {
    result += digits[data[i] >> 4];
    result += digits[data[i] & 15];
  }
  return result;
}
std::string derive(const std::string &password, const std::string &salt) {
  unsigned char hash[32];
  if (!PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                         reinterpret_cast<const unsigned char *>(salt.data()),
                         static_cast<int>(salt.size()), 210000, EVP_sha256(), sizeof(hash), hash))
    throw std::runtime_error("credential_derivation_failed");
  return hex(hash, sizeof(hash));
}
bool equal(const std::string &a, const std::string &b) {
  return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
Response response(http::status status, const std::string &body,
                  const std::string &type = "application/json") {
  Response r{status, 11};
  r.set(http::field::content_type, type);
  r.set(http::field::cache_control, "no-store");
  r.set("X-Content-Type-Options", "nosniff");
  r.set("Referrer-Policy", "no-referrer");
  r.set("Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' "
        "data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none'; base-uri 'none'; "
        "form-action 'self'");
  r.keep_alive(false);
  r.body() = body;
  r.prepare_payload();
  return r;
}
Response error(http::status status, const std::string &code) {
  return response(status, Json{{"error", {{"code", code}}}}.dump());
}
std::string cookie(const Request &r) {
  auto value = std::string(r[http::field::cookie]);
  std::string result;
  while (!value.empty()) {
    auto end = value.find(';');
    auto part = value.substr(0, end);
    auto begin = part.find_first_not_of(' ');
    if (begin != std::string::npos)
      part.erase(0, begin);
    if (part.starts_with("graphlab_session=")) {
      if (!result.empty())
        return {};
      result = part.substr(17);
    }
    if (end == std::string::npos)
      break;
    value.erase(0, end + 1);
  }
  return result;
}
} // namespace
void initialize_auth(const std::filesystem::path &file) {
  auto password = random_hex(32), salt = random_hex(16);
  auto body = Json{{"apiVersion", "graphlab.auth/v1"},
                   {"algorithm", "pbkdf2-sha256"},
                   {"iterations", 210000},
                   {"salt", salt},
                   {"verifier", derive(password, salt)}}
                  .dump(2) +
              "\n";
  int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create new credential file");
  auto n = write(fd, body.data(), body.size());
  bool failed = n != static_cast<ssize_t>(body.size()) || fsync(fd) != 0;
  close(fd);
  if (failed) {
    unlink(file.c_str());
    throw std::runtime_error("credential_write_failed");
  }
  std::cout << password << '\n';
}
Router::Router(std::string host, std::string socket, uid_t uid, const std::filesystem::path &auth,
               const std::filesystem::path &assets, std::chrono::seconds lifetime)
    : host_(std::move(host)), socket_(std::move(socket)), agent_uid_(uid), lifetime_(lifetime) {
  struct stat s{};
  if (lstat(auth.c_str(), &s) || !S_ISREG(s.st_mode) || s.st_uid != geteuid() || (s.st_mode & 0077))
    throw std::runtime_error("credential file must be owned and mode 0600");
  verifier_ = load(auth);
  if (verifier_.value("apiVersion", "") != "graphlab.auth/v1" ||
      verifier_.value("algorithm", "") != "pbkdf2-sha256" ||
      verifier_.value("iterations", 0) != 210000 || !verifier_.contains("salt") ||
      !verifier_["salt"].is_string() || !verifier_.contains("verifier") ||
      !verifier_["verifier"].is_string() || verifier_["salt"].get<std::string>().size() != 32 ||
      verifier_["verifier"].get<std::string>().size() != 64)
    throw std::runtime_error("invalid credential verifier");
  std::size_t total = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(assets)) {
    if (entry.is_symlink())
      throw std::runtime_error("symlinks forbidden in web assets");
    if (!entry.is_regular_file())
      continue;
    auto extension = entry.path().extension().string();
    std::string type = extension == ".html"  ? "text/html; charset=utf-8"
                       : extension == ".js"  ? "text/javascript"
                       : extension == ".css" ? "text/css"
                                             : "";
    if (type.empty())
      continue;
    total += entry.file_size();
    if (total > 16 * 1024 * 1024)
      throw std::runtime_error("web assets exceed 16 MiB");
    std::ifstream input(entry.path(), std::ios::binary);
    std::string content{std::istreambuf_iterator<char>(input), {}};
    assets_["/" + std::filesystem::relative(entry.path(), assets).generic_string()] = {type,
                                                                                       content};
  }
  if (!assets_.contains("/index.html"))
    throw std::runtime_error("build console/web first: index.html missing");
}
Response Router::handle(const Request &r) {
  // Reject ambiguous security headers before any route, including static assets.
  for (auto name : {http::field::host, http::field::origin, http::field::cookie})
    if (r.count(name) > 1)
      return error(http::status::bad_request, "duplicate_header");
  if (r[http::field::host] != host_)
    return error(http::status::forbidden, "host_denied");
  auto origin = std::string(r[http::field::origin]);
  if ((!origin.empty() && origin != "http://" + host_) || r["Sec-Fetch-Site"] == "cross-site")
    return error(http::status::forbidden, "origin_denied");
  if (r.method() != http::verb::get && origin != "http://" + host_)
    return error(http::status::forbidden, "origin_required");
  if (!r[http::field::upgrade].empty())
    return error(http::status::bad_request, "upgrade_unsupported");
  auto path = std::string(r.target());
  if (path.size() > 256 || path.find_first_of("?%\\#") != std::string::npos ||
      path.find("..") != std::string::npos)
    return error(http::status::bad_request, "invalid_path");
  auto now = std::chrono::steady_clock::now();
  std::erase_if(sessions_, [&](const auto &entry) { return entry.second.expires <= now; });
  if (path == "/api/v1/login" && r.method() == http::verb::post) {
    if (now - attempt_ < std::chrono::milliseconds(500))
      return error(http::status::too_many_requests, "login_rate_limited");
    attempt_ = now;
    auto body = lab_support::parse_document(r.body());
    if (!body || !Json::accept(r.body()) || body->size() != 1 || !body->contains("password") ||
        !(*body)["password"].is_string() || (*body)["password"].get<std::string>().size() > 256)
      return error(http::status::bad_request, "invalid_login");
    if (!equal(derive((*body)["password"], verifier_["salt"]), verifier_["verifier"]))
      return error(http::status::unauthorized, "invalid_credentials");
    if (sessions_.size() >= 32)
      return error(http::status::too_many_requests, "session_limit");
    auto token = random_hex(32), csrf = random_hex(32);
    sessions_[token] = {csrf, now + lifetime_};
    auto result = response(http::status::ok,
                           Json{{"csrf", csrf}, {"expiresInSeconds", lifetime_.count()}}.dump());
    result.set(http::field::set_cookie, "graphlab_session=" + token +
                                            "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
                                            std::to_string(lifetime_.count()));
    return result;
  }
  if (!path.starts_with("/api/")) {
    if (r.method() != http::verb::get)
      return error(http::status::method_not_allowed, "read_only");
    auto it = assets_.find(path == "/" ? "/index.html" : path);
    if (it == assets_.end())
      return error(http::status::not_found, "not_found");
    return response(http::status::ok, it->second.second, it->second.first);
  }
  auto session = sessions_.find(cookie(r));
  if (session == sessions_.end())
    return error(http::status::unauthorized, "authentication_required");
  if (path == "/api/v1/session" && r.method() == http::verb::get)
    return response(http::status::ok,
                    Json{{"csrf", session->second.csrf}, {"readOnly", true}}.dump());
  if (path == "/api/v1/logout" && r.method() == http::verb::post) {
    if (r.count("X-CSRF-Token") != 1 ||
        !equal(std::string(r["X-CSRF-Token"]), session->second.csrf))
      return error(http::status::forbidden, "csrf_denied");
    sessions_.erase(session);
    auto result = response(http::status::ok, "{}");
    result.set(http::field::set_cookie,
               "graphlab_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    return result;
  }
  if (path == "/api/v1/runs" || path.starts_with("/api/v1/runs/") ||
      path.starts_with("/api/v1/jobs/")) {
    try {
      auto capabilities = rpc(socket_, agent_uid_, "capabilities");
      if (!capabilities.value("execution", false))
        return error(http::status::method_not_allowed, "read_only");
      Json params = Json::object();
      std::string method;
      if (r.method() == http::verb::post || r.method() == http::verb::delete_) {
        if (r.count("X-CSRF-Token") != 1 ||
            !equal(std::string(r["X-CSRF-Token"]), session->second.csrf))
          return error(http::status::forbidden, "csrf_denied");
        auto parsed = lab_support::parse_document(r.body());
        if (!parsed || !Json::accept(r.body()))
          return error(http::status::bad_request, "invalid_json");
        params = *parsed;
        if (r.method() == http::verb::delete_ && path.starts_with("/api/v1/runs/") &&
            path.find("/faults/", 13) != std::string::npos) {
          auto pos = path.find("/faults/", 13);
          params["runId"] = path.substr(13, pos - 13);
          params["faultId"] = path.substr(pos + 8);
          method = "fault.remove";
        } else if (r.method() == http::verb::delete_) {
          return error(http::status::method_not_allowed, "unsupported_method");
        } else if (path.starts_with("/api/v1/runs/") &&
                   (path.ends_with("/faults") || path.ends_with("/faults/preview") ||
                    path.ends_with("/faults/remove") || path.ends_with("/telemetry/query"))) {
          auto slash = path.find('/', 13);
          params["runId"] = path.substr(13, slash - 13);
          method = path.ends_with("/telemetry/query") ? "telemetry"
                   : path.ends_with("/preview")       ? "fault.preview"
                   : path.ends_with("/remove")        ? "fault.remove"
                                                      : "fault.apply";
        } else if (path.starts_with("/api/v1/runs/") && path.ends_with("/terminal")) {
          method = "terminal";
          params["runId"] = path.substr(13, path.size() - 13 - 9);
          params["owner"] = lab_support::digest(Json(cookie(r)));
        } else if (path == "/api/v1/runs")
          method = "start";
        else if (path.starts_with("/api/v1/runs/") && path.ends_with("/operations")) {
          method = "operate";
          params["runId"] = path.substr(13, path.size() - 13 - 11);
        } else if (path.starts_with("/api/v1/jobs/") && path.ends_with("/cancel")) {
          method = "cancel";
          params["id"] = path.substr(13, path.size() - 13 - 7);
        } else
          return error(http::status::not_found, "not_found");
      } else if (r.method() == http::verb::get) {
        auto artifact = path.find("/artifacts");
        if (path.starts_with("/api/v1/runs/") &&
            (path.ends_with("/telemetry") || path.ends_with("/timeline") ||
             path.ends_with("/faults"))) {
          auto slash = path.find('/', 13);
          params["runId"] = path.substr(13, slash - 13);
          method = path.substr(slash + 1);
        } else if (path.starts_with("/api/v1/runs/") && artifact != std::string::npos) {
          params["runId"] = path.substr(13, artifact - 13);
          if (path.size() == artifact + 10)
            method = "artifacts";
          else {
            auto chunks = path.find("/chunks/", artifact + 11);
            if (chunks == std::string::npos)
              return error(http::status::not_found, "not_found");
            method = "artifact";
            params["id"] = path.substr(artifact + 11, chunks - artifact - 11);
            params["offset"] = path.substr(chunks + 8);
          }
        } else if (path == "/api/v1/runs")
          method = "runs";
        else {
          method = path.starts_with("/api/v1/jobs/") ? "job" : "run";
          params["id"] = path.substr(13);
        }
      } else
        return error(http::status::method_not_allowed, "unsupported_method");
      auto value = rpc(socket_, agent_uid_, method, params);
      return response((r.method() == http::verb::post || r.method() == http::verb::delete_)
                          ? http::status::accepted
                          : http::status::ok,
                      value.dump());
    } catch (const runtime::Failure &e) {
      return error(static_cast<http::status>(e.status), e.what());
    } catch (...) {
      return error(http::status::service_unavailable, "agent_unavailable");
    }
  }
  if (r.method() != http::verb::get)
    return error(http::status::method_not_allowed, "read_only");
  std::string method;
  Json params = Json::object();
  if (path == "/api/v1/capabilities")
    method = "capabilities";
  else if (path == "/api/v1/topologies")
    method = "topologies";
  else if (path == "/api/v1/diagnostics")
    method = "diagnostics";
  else if (path.starts_with("/api/v1/topologies/") && path.ends_with("/inventory")) {
    auto hash = path.substr(std::string_view("/api/v1/topologies/").size(),
                            path.size() - std::string_view("/api/v1/topologies/").size() -
                                std::string_view("/inventory").size());
    if (hash.size() != 71 || !hash.starts_with("sha256:") ||
        hash.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos)
      return error(http::status::bad_request, "invalid_hash");
    method = "inventory";
    params["hash"] = hash;
  } else
    return error(http::status::not_found, "not_found");
  try {
    return response(http::status::ok, rpc(socket_, agent_uid_, method, params).dump());
  } catch (const std::exception &e) {
    return error(std::string(e.what()) == "agent_rejected" ? http::status::not_found
                                                           : http::status::service_unavailable,
                 std::string(e.what()) == "agent_rejected" ? "not_found" : "agent_unavailable");
  }
}
namespace {
struct TerminalSocket : std::enable_shared_from_this<TerminalSocket> {
  boost::beast::websocket::stream<boost::beast::tcp_stream> socket;
  boost::beast::flat_buffer buffer;
  Router &router;
  Request credentials;
  std::shared_ptr<std::size_t> active;
  std::vector<std::pair<bool, std::string>> output;
  std::size_t sent = 0, frames = 0;
  std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
  TerminalSocket(boost::beast::tcp_stream stream, Router &r, Request request,
                 std::shared_ptr<std::size_t> a)
      : socket(std::move(stream)), router(r), credentials(std::move(request)),
        active(std::move(a)) {
    ++*active;
  }
  ~TerminalSocket() { --*active; }
  void start() {
    socket.next_layer().expires_never();
    socket.set_option(boost::beast::websocket::stream_base::timeout{
        std::chrono::seconds(3), std::chrono::seconds(20), true});
    socket.read_message_max(4096);
    socket.set_option(boost::beast::websocket::stream_base::decorator(
        [](boost::beast::websocket::response_type &r) {
          r.set(http::field::sec_websocket_protocol, "graphlab.terminal.v1");
        }));
    auto self = shared_from_this();
    socket.async_accept(credentials, [self](auto ec) {
      if (!ec)
        self->read();
    });
  }
  void read() {
    auto self = shared_from_this();
    socket.async_read(buffer, [self](auto ec, auto) {
      if (ec)
        return;
      auto now = std::chrono::steady_clock::now();
      if (now - self->window >= std::chrono::seconds(1)) {
        self->window = now;
        self->frames = 0;
      }
      if (++self->frames > 64)
        return self->close();
      self->output.clear();
      self->sent = 0;
      try {
        if (!self->socket.got_text())
          throw std::runtime_error("control_frame_required");
        auto message = Json::parse(boost::beast::buffers_to_string(self->buffer.data()));
        auto id = message.at("runId").get<std::string>();
        if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos)
          throw std::runtime_error("invalid_run");
        Request request = self->credentials;
        request.erase(http::field::upgrade);
        request.method(http::verb::post);
        request.target("/api/v1/runs/" + id + "/terminal");
        request.set("X-CSRF-Token", message.at("csrf").get<std::string>());
        message.erase("csrf");
        message.erase("runId");
        request.body() = message.dump();
        auto result = self->router.handle(request);
        auto body = Json::parse(result.body());
        if (result.result() == http::status::accepted && body.contains("records")) {
          // Each binary frame is sequence:u64, outputOffset:u64, type:u32, payloadLength:u32 (big
          // endian), then opaque payload.
          for (const auto &r : body["records"]) {
            std::string frame;
            auto put = [&](std::uint64_t n, int width) {
              for (int i = width - 1; i >= 0; --i)
                frame.push_back(char(n >> (8 * i)));
            };
            auto bytes = terminal::decode(r["base64"]);
            put(std::stoull(r["sequence"].get<std::string>()), 8);
            put(std::stoull(r["offset"].get<std::string>()), 8);
            put(r["type"], 4);
            put(bytes.size(), 4);
            frame += bytes;
            self->output.emplace_back(false, std::move(frame));
          }
          body.erase("records");
        }
        self->output.emplace_back(true,
                                  Json{{"status", result.result_int()}, {"result", body}}.dump());
      } catch (...) {
        self->output.emplace_back(true, "{\"status\":400,\"error\":\"invalid_terminal_frame\"}");
      }
      self->buffer.consume(self->buffer.size());
      std::size_t size = 0;
      for (const auto &f : self->output)
        size += f.second.size();
      if (size > 1024 * 1024)
        return self->close();
      self->write();
    });
  }
  void write() {
    if (sent == output.size())
      return read();
    socket.text(output[sent].first);
    auto self = shared_from_this();
    socket.async_write(asio::buffer(output[sent].second), [self](auto ec, auto) {
      if (ec)
        return;
      ++self->sent;
      self->write();
    });
  }
  void close() {
    boost::system::error_code ec;
    socket.next_layer().socket().close(ec);
  }
};
struct HttpConnection : std::enable_shared_from_this<HttpConnection> {
  boost::beast::tcp_stream stream;
  boost::beast::flat_buffer buffer;
  http::request_parser<http::string_body> parser;
  Response output;
  Router &router;
  std::shared_ptr<std::size_t> active;
  HttpConnection(asio::ip::tcp::socket s, Router &r, std::shared_ptr<std::size_t> a)
      : stream(std::move(s)), router(r), active(std::move(a)) {
    ++*active;
    parser.body_limit(4096);
    parser.header_limit(8192);
  }
  ~HttpConnection() { --*active; }
  void start() {
    auto self = shared_from_this();
    stream.expires_after(std::chrono::seconds(5));
    http::async_read(stream, buffer, parser, [self](auto ec, auto) {
      if (ec)
        return;
      try {
        if (boost::beast::websocket::is_upgrade(self->parser.get()) &&
            self->parser.get().target() == "/api/v1/terminal") {
          auto request = self->parser.get();
          auto authorized = request;
          authorized.erase(http::field::upgrade);
          authorized.target("/api/v1/session");
          self->output = self->router.handle(authorized);
          if (request[http::field::origin].empty() ||
              request[http::field::sec_websocket_protocol] != "graphlab.terminal.v1")
            self->output = error(http::status::forbidden, "terminal_origin_or_protocol_denied");
          if (self->output.result() == http::status::ok) {
            std::make_shared<TerminalSocket>(std::move(self->stream), self->router,
                                             std::move(request), self->active)
                ->start();
            return;
          }
        } else
          self->output = self->router.handle(self->parser.get());
      } catch (...) {
        self->output = error(http::status::internal_server_error, "internal_error");
      }
      self->stream.expires_after(std::chrono::seconds(5));
      http::async_write(self->stream, self->output, [self](auto, auto) {
        boost::system::error_code ignored;
        self->stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
      });
    });
  }
};
} // namespace
void run_http(unsigned short port, Router &router) {
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), port});
  auto active = std::make_shared<std::size_t>(0);
  std::function<void()> accept;
  accept = [&] {
    acceptor.async_accept([&](auto ec, asio::ip::tcp::socket socket) {
      if (!ec && *active < 64)
        std::make_shared<HttpConnection>(std::move(socket), router, active)->start();
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
