#include <boost/beast.hpp>
#include <fstream>
#include <graphlab/console.hpp>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace graphlab::console;
namespace {
int checks = 0;
void check(bool condition, const std::string &name) {
  if (!condition)
    throw std::runtime_error(name);
  ++checks;
  std::cout << "PASS " << name << '\n';
}
struct Child {
  pid_t pid = -1;
  ~Child() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
    }
  }
};
void start(Child &child, const std::string &binary, const std::vector<std::string> &args) {
  child.pid = fork();
  if (child.pid == 0) {
    std::vector<char *> argv{const_cast<char *>(binary.c_str())};
    for (const auto &a : args)
      argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execv(binary.c_str(), argv.data());
    _exit(127);
  }
  if (child.pid < 0)
    throw std::runtime_error("fork failed");
}
Response request(unsigned short port, http::verb verb, const std::string &path,
                 const std::string &body = "", const std::string &cookie = "",
                 const std::string &origin = "default", const std::string &host = "default",
                 const std::string &csrf = "") {
  asio::io_context io;
  asio::ip::tcp::socket socket(io);
  socket.connect({asio::ip::make_address("127.0.0.1"), port});
  Request r{verb, path, 11};
  auto authority = "127.0.0.1:" + std::to_string(port);
  r.set(http::field::host, host == "default" ? authority : host);
  if (origin != "none")
    r.set(http::field::origin, origin == "default" ? "http://" + authority : origin);
  if (!cookie.empty())
    r.set(http::field::cookie, cookie);
  if (!csrf.empty())
    r.set("X-CSRF-Token", csrf);
  r.body() = body;
  r.prepare_payload();
  http::write(socket, r);
  boost::beast::flat_buffer buffer;
  Response response;
  http::read(socket, buffer, response);
  return response;
}
} // namespace
int main(int argc, char **argv) {
  std::filesystem::path work;
  try {
    if (argc != 4)
      throw std::runtime_error("requires source, agent, API paths");
    auto source = std::filesystem::path(argv[1]);
    char temporary[] = "/tmp/graphlab-m1-XXXXXX";
    auto created = mkdtemp(temporary);
    if (!created)
      throw std::runtime_error("mkdtemp failed");
    work = created;
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{work};
    auto socket = (work / "agent.sock").string();
    Catalog catalog(source / "topologies", source / "topologies/artifacts.lock.json");
    auto call = [&](const std::string &method, Json params = Json::object()) {
      return catalog.dispatch(
          {{"apiVersion", "graphlab.rpc/v1"}, {"method", method}, {"params", params}});
    };
    auto listing = call("topologies");
    check(listing["items"].size() == 8, "all eight saved graph fixtures loaded");
    std::filesystem::create_directory(work / "fixtures");
    std::filesystem::copy_file(source / "topologies/chain.yaml", work / "fixtures/chain.yaml");
    std::ofstream(work / "fixtures/._chain.yaml") << "OS metadata, not a topology";
    Catalog sidecars(work / "fixtures", source / "topologies/artifacts.lock.json");
    check(sidecars.dispatch({{"apiVersion", "graphlab.rpc/v1"},
                             {"method", "topologies"},
                             {"params", Json::object()}})["items"]
                  .size() == 1,
          "OS metadata sidecars ignored");
    for (const auto &item : listing["items"]) {
      auto i = call("inventory", {{"hash", item["hash"]}});
      check(i["nodes"].size() == item["nodes"] && i["edges"].size() == item["edges"],
            "inventory preserves " + item["id"].get<std::string>());
      check(i["runtimeObservedAt"].is_null() && i["runtimeFreshness"] == "unknown",
            "unknown freshness " + item["id"].get<std::string>());
      for (const auto &n : i["nodes"])
        check(n["runtime"]["identity"].is_null(),
              "no invented runtime identity " + n["id"].get<std::string>());
    }
    for (const auto &bad :
         {Json{{"apiVersion", "graphlab.rpc/v9"},
               {"method", "topologies"},
               {"params", Json::object()}},
          Json{{"apiVersion", "graphlab.rpc/v1"}, {"method", "apply"}, {"params", Json::object()}},
          Json{{"apiVersion", "graphlab.rpc/v1"},
               {"method", "topologies"},
               {"params", {{"path", "/etc/passwd"}}}}}) {
      bool rejected = false;
      try {
        catalog.dispatch(bad);
      } catch (...) {
        rejected = true;
      }
      check(rejected, "invalid RPC rejected");
    }
    Child agent;
    start(agent, argv[2],
          {"--socket", socket, "--topologies", (source / "topologies").string(), "--lock",
           (source / "topologies/artifacts.lock.json").string()});
    for (int n = 0; n < 100 && !std::filesystem::exists(socket); ++n)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(rpc(socket, geteuid(), "capabilities")["readOnly"] == true, "real Unix RPC read surface");
    bool denied = false;
    try {
      rpc(socket, geteuid() + 1, "capabilities");
    } catch (...) {
      denied = true;
    }
    check(denied, "client rejects wrong agent UID");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
      throw std::runtime_error("socketpair");
    require_peer(pair[0], geteuid());
    denied = false;
    try {
      require_peer(pair[0], geteuid() + 1);
    } catch (...) {
      denied = true;
    }
    close(pair[0]);
    close(pair[1]);
    check(denied, "server peer policy rejects actual mismatched credentials");
    {
      asio::io_context io;
      asio::local::stream_protocol::socket s(io);
      s.connect(asio::local::stream_protocol::endpoint(socket));
      std::array<unsigned char, 4> huge{255, 255, 255, 255};
      asio::write(s, asio::buffer(huge));
      char byte;
      boost::system::error_code ec;
      s.read_some(asio::buffer(&byte, 1), ec);
      check(bool(ec), "oversized RPC closes connection before allocation");
    }
    for (const auto &payload : {std::string("{"), std::string("[]"),
                                std::string("{\"apiVersion\":\"graphlab.rpc/v999\"}"),
                                std::string("{\"secret\":\"M6_DO_NOT_ECHO\",\"x\":NaN}"),
                                std::string(4096, '['), std::string("\xff\0\xff", 3)}) {
      asio::io_context io;
      asio::local::stream_protocol::socket s(io);
      s.connect(asio::local::stream_protocol::endpoint(socket));
      std::string frame(4, '\0');
      for (int i = 0; i < 4; ++i)
        frame[i] = static_cast<char>(payload.size() >> (24 - 8 * i));
      frame += payload;
      asio::write(s, asio::buffer(frame));
      std::array<unsigned char, 4> header{};
      asio::read(s, asio::buffer(header));
      auto size = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16) |
                  (std::uint32_t(header[2]) << 8) | header[3];
      check(size > 0 && size <= 4096, "malformed RPC response bounded");
      std::string body(size, '\0');
      asio::read(s, asio::buffer(body));
      check(Json::parse(body).at("ok") == false && body.find("M6_DO_NOT_ECHO") == std::string::npos,
            "malformed RPC denied without echoing supplied secrets");
    }
    {
      asio::io_context io;
      asio::local::stream_protocol::socket s(io);
      s.connect(asio::local::stream_protocol::endpoint(socket));
      auto began = std::chrono::steady_clock::now();
      char byte;
      boost::system::error_code ec;
      s.read_some(asio::buffer(&byte, 1), ec);
      check(bool(ec) && std::chrono::steady_clock::now() - began < std::chrono::seconds(5),
            "idle RPC client bounded by deadline");
    }
    std::ostringstream credential;
    auto old = std::cout.rdbuf(credential.rdbuf());
    initialize_auth(work / "auth.json");
    std::cout.rdbuf(old);
    auto password = credential.str();
    password.pop_back();
    check(load(work / "auth.json").dump().find(password) == std::string::npos,
          "credential persisted as verifier only");
    std::filesystem::create_directory(work / "assets");
    std::ofstream(work / "assets/index.html") << "<!doctype html><title>M1</title>";
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    auto port = reservation.local_endpoint().port();
    reservation.close();
    Child api;
    start(api, argv[3],
          {"--socket", socket, "--auth", (work / "auth.json").string(), "--assets",
           (work / "assets").string(), "--port", std::to_string(port)});
    bool ready = false;
    for (int n = 0; n < 100; ++n) {
      try {
        request(port, http::verb::get, "/");
        ready = true;
        break;
      } catch (...) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    check(ready, "HTTP service starts");
    bool oversized = false;
    try {
      request(port, http::verb::post, "/api/v1/login", std::string(8192, 'x'));
    } catch (const boost::system::system_error &) {
      oversized = true;
    }
    check(oversized, "oversized HTTP body rejected before routing");
    check(request(port, http::verb::get, "/api/v1/topologies").result_int() == 401,
          "unauthenticated API denied");
    check(request(port, http::verb::get, "/", "", "", "http://hostile.invalid").result_int() == 403,
          "hostile Origin denied");
    check(request(port, http::verb::get, "/", "", "", "none", "hostile.invalid").result_int() ==
              403,
          "hostile Host denied");
    check(request(port, http::verb::post, "/api/v1/login", "{}", "", "none").result_int() == 403,
          "login requires Origin");
    check(
        request(port, http::verb::post, "/api/v1/login", "{\"password\":\"wrong\"}").result_int() ==
            401,
        "invalid credential denied");
    check(request(port, http::verb::post, "/api/v1/login", "{}").result_int() == 429,
          "login attempts bounded");
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    auto login =
        request(port, http::verb::post, "/api/v1/login", Json{{"password", password}}.dump());
    check(login.result_int() == 200, "valid login succeeds");
    auto set_cookie = std::string(login[http::field::set_cookie]),
         cookie = set_cookie.substr(0, set_cookie.find(';'));
    check(set_cookie.find("HttpOnly") != std::string::npos &&
              set_cookie.find("SameSite=Strict") != std::string::npos,
          "session cookie policy");
    auto csrf = Json::parse(login.body())["csrf"].get<std::string>();
    check(request(port, http::verb::get, "/api/v1/topologies", "", cookie).result_int() == 200,
          "authenticated catalog through HTTP and agent");
    auto hash = listing["items"][0]["hash"].get<std::string>();
    check(request(port, http::verb::get, "/api/v1/topologies/" + hash + "/inventory", "", cookie)
                  .result_int() == 200,
          "inventory routing by immutable hash");
    check(request(port, http::verb::get, "/api/v1/diagnostics", "", cookie).result_int() == 200,
          "host diagnostics available");
    check(request(port, http::verb::post, "/api/v1/runs", "{}", cookie).result_int() == 405,
          "execution route rejected");
    check(request(port, http::verb::get, "/../auth.json", "", cookie).result_int() == 400,
          "asset traversal denied");
    check(request(port, http::verb::post, "/api/v1/logout", "{}", cookie).result_int() == 403,
          "logout CSRF enforced");
    check(
        request(port, http::verb::post, "/api/v1/logout", "{}", cookie, "default", "default", csrf)
                .result_int() == 200,
        "valid logout succeeds");
    check(request(port, http::verb::get, "/api/v1/topologies", "", cookie).result_int() == 401,
          "revoked cookie denied");
    Router short_session("127.0.0.1:8088", socket, geteuid(), work / "auth.json", work / "assets",
                         std::chrono::seconds(0));
    Request local{http::verb::post, "/api/v1/login", 11};
    local.set(http::field::host, "127.0.0.1:8088");
    local.set(http::field::origin, "http://127.0.0.1:8088");
    local.body() = Json{{"password", password}}.dump();
    auto expired = short_session.handle(local);
    check(expired.result_int() == 200, "short-lived session created");
    local.method(http::verb::get);
    local.target("/api/v1/session");
    auto expiry_cookie = std::string(expired[http::field::set_cookie]);
    local.set(http::field::cookie, expiry_cookie.substr(0, expiry_cookie.find(';')));
    check(short_session.handle(local).result_int() == 401, "expired session denied");
    local.insert(http::field::host, "hostile.invalid");
    check(short_session.handle(local).result_int() == 400, "duplicate Host rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    login = request(port, http::verb::post, "/api/v1/login", Json{{"password", password}}.dump());
    set_cookie = std::string(login[http::field::set_cookie]);
    cookie = set_cookie.substr(0, set_cookie.find(';'));
    kill(agent.pid, SIGTERM);
    waitpid(agent.pid, nullptr, 0);
    agent.pid = -1;
    check(request(port, http::verb::get, "/api/v1/topologies", "", cookie).result_int() == 503,
          "agent outage has no direct fallback");
    check(!std::filesystem::exists(socket), "agent removes own socket on orderly stop");
    std::cout << checks << " M1 checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
