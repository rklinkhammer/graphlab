#pragma once
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <lab_support/contracts.hpp>
#include <map>
#include <sys/types.h>

namespace graphlab::console {
using lab_support::Json;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
constexpr std::size_t max_frame = 16 * 1024 * 1024;
Json load(const std::filesystem::path &path);
std::string random_hex(std::size_t bytes);
std::string timestamp();
uid_t peer_uid(int fd);
void require_peer(int fd, uid_t expected);
Json rpc(const std::string &socket, uid_t expected, const std::string &method,
         const Json &params = Json::object());
class Catalog {
  std::map<std::string, Json> revisions_;
  std::string loaded_;
  Json artifacts_;

public:
  Catalog(const std::filesystem::path &directory, const std::filesystem::path &lock);
  Json dispatch(const Json &request) const;
  Json resolve(const std::string &hash) const;
};
void run_agent(const std::string &socket, uid_t allowed, const Catalog &catalog,
               std::function<Json(const Json &, uid_t)> control = {});
void initialize_auth(const std::filesystem::path &file);
class Router {
  struct Session {
    std::string csrf;
    std::chrono::steady_clock::time_point expires;
  };
  std::map<std::string, Session> sessions_;
  Json verifier_;
  std::map<std::string, std::pair<std::string, std::string>> assets_;
  std::string host_, socket_;
  uid_t agent_uid_;
  std::chrono::steady_clock::time_point attempt_{};
  std::chrono::seconds lifetime_;

public:
  Router(std::string host, std::string socket, uid_t agent_uid, const std::filesystem::path &auth,
         const std::filesystem::path &assets,
         std::chrono::seconds lifetime = std::chrono::minutes(30));
  Response handle(const Request &request);
};
void run_http(unsigned short port, Router &router);
} // namespace graphlab::console
