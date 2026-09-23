#include <barrier>
#include <fstream>
#include <future>
#include <graphlab/runtime.hpp>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace graphlab::runtime;
namespace {
void check(bool b, const std::string &why) {
  if (!b)
    throw std::runtime_error(why);
  std::cout << "PASS " << why << '\n';
}
Json call(Engine &e, const std::string &method, Json params = Json::object()) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", method}, {"params", params}},
                    geteuid());
}
Json wait(Engine &e, const Json &accepted) {
  for (int i = 0; i < 1000; ++i) {
    auto j = call(e, "job", {{"id", accepted["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("job wait timeout");
}
struct Simulated : Backend {
  int fail = -1, index = 0;
  bool after = false;
  bool fail_remove = false;
  int delay_ms = 0;
  std::map<std::string, Json> live;
  Json logs(const Json &run, const Json &resource) override {
    return {{"runId", run["id"]}, {"node", resource["logical"]}};
  }
  void preflight(const Json &, const Json &) override {}
  Json prepare(const Json &run, const Json &r) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    auto key = run["id"].get<std::string>() + r["key"].get<std::string>();
    if (index++ == fail) {
      if (after)
        live[key] = r;
      throw Failure("injected_failure");
    }
    live[key] = r;
    return {{"id", key}};
  }
  void remove(const Json &run, const Json &r) override {
    if (fail_remove)
      throw Failure("ovs_inventory_unavailable", 503);
    live.erase(run["id"].get<std::string>() + r["key"].get<std::string>());
  }
  void activate(const Json &) override {}
  void gate(const Json &, const std::string &) override {}
  Json observe(const Json &) override { return {{"observedAt", graphlab::console::timestamp()}}; }
};
void write(const std::filesystem::path &p, const Json &j) { std::ofstream(p) << j.dump(2) << '\n'; }
void client_race(const std::filesystem::path &directory, const graphlab::console::Catalog &catalog,
                 const Json &start, const std::filesystem::path &cli) {
  using namespace graphlab::console;
  auto state = directory / "rpc-state";
  std::filesystem::create_directory(state);
  chmod(state.c_str(), 0700);
  auto socket = (directory / "race.sock").string();
  auto child = fork();
  if (child == 0) {
    Simulated backend;
    backend.delay_ms = 250;
    Engine engine(state, backend, catalog);
    run_agent(socket, geteuid(), catalog, [&](const Json &r, uid_t uid) {
      if (r["method"] == "capabilities")
        return Json{{"execution", true}};
      return engine.dispatch(r, uid);
    });
    _exit(0);
  }
  if (child < 0)
    throw std::runtime_error("fork failed");
  struct Child {
    pid_t pid;
    ~Child() {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
    }
  } cleanup{child};
  for (int i = 0; i < 100 && !std::filesystem::exists(socket); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::ostringstream credential;
  auto previous = std::cout.rdbuf(credential.rdbuf());
  try {
    initialize_auth(directory / "auth.json");
  } catch (...) {
    std::cout.rdbuf(previous);
    throw;
  }
  std::cout.rdbuf(previous);
  auto password = credential.str();
  while (!password.empty() && password.back() == '\n')
    password.pop_back();
  auto assets = directory / "assets";
  std::filesystem::create_directory(assets);
  std::ofstream(assets / "index.html") << "<!doctype html><title>Test</title>";
  Router router("127.0.0.1:18088", socket, geteuid(), directory / "auth.json", assets);
  Request login{http::verb::post, "/api/v1/login", 11};
  login.set(http::field::host, "127.0.0.1:18088");
  login.set(http::field::origin, "http://127.0.0.1:18088");
  login.body() = Json{{"password", password}}.dump();
  auto logged = router.handle(login);
  check(logged.result_int() == 200, "mutation test authenticates HTTP session");
  Request request{http::verb::post, "/api/v1/runs", 11};
  request.set(http::field::host, "127.0.0.1:18088");
  request.set(http::field::origin, "http://127.0.0.1:18088");
  auto cookie = std::string(logged[http::field::set_cookie]);
  request.set(http::field::cookie, cookie.substr(0, cookie.find(';')));
  request.set("X-CSRF-Token", Json::parse(logged.body())["csrf"].get<std::string>());
  request.body() = start.dump();
  write(directory / "start.json", start);
  std::barrier ready(2);
  auto command = std::async(std::launch::async, [&] {
    ready.arrive_and_wait();
    return process({cli.string(), "control", "--socket", socket, "--agent-uid",
                    std::to_string(geteuid()), "start", (directory / "start.json").string()});
  });
  ready.arrive_and_wait();
  auto response = router.handle(request);
  auto result = command.get();
  check(response.result_int() == 202 && result.code == 0 &&
            Json::parse(response.body()) == Json::parse(result.output),
        "HTTP and actual CLI concurrent retries share one job");
  auto competing = start;
  competing["idempotencyKey"] = "competing-http-cli";
  request.body() = competing.dump();
  write(directory / "start.json", competing);
  check(router.handle(request).result_int() == 409, "HTTP concurrent mutation conflicts");
  auto rejected =
      process({cli.string(), "control", "--socket", socket, "--agent-uid",
               std::to_string(geteuid()), "start", (directory / "start.json").string()});
  check(rejected.code != 0 && rejected.output.find("operation_in_progress") != std::string::npos,
        "CLI concurrent mutation conflicts with same authority");
}
Json fixture(const std::filesystem::path &source, const std::filesystem::path &dir,
             const std::string &image) {
  auto lock = graphlab::console::load(source / "topologies/artifacts.lock.json");
  lock["workloads"].erase("guest-linux");
  for (auto &[id, e] : lock["workloads"].items())
    e["image"] = "graphlab.local/" + id + "@" + image;
  auto t = graphlab::console::load(source / "topologies/triangle.yaml");
  t["id"] = "m2-network";
  t["artifactLock"] = lab_support::digest(lock);
  t["capture"]["required"] = false;
  t["nodes"].erase("g");
  t["management"] = {{"networks", Json::object()}};
  t["managementAttachments"] = Json::array();
  t["nodes"]["a"]["addresses"] = {{"data0", "10.231.17.1/24"}};
  t["nodes"]["b"]["addresses"] = {{"data0", "10.231.17.2/24"}};
  Json edges = Json::array();
  for (const auto &e : t["edges"])
    if (e["id"] != "g-s")
      edges.push_back(e);
  t["edges"] = edges;
  write(dir / "m2.yaml", t);
  write(dir / "artifacts.lock.json", lock);
  return t;
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 5 && std::string(argv[1]) == "--fixtures") {
      std::filesystem::create_directories(argv[3]);
      fixture(argv[2], argv[3], argv[4]);
      return 0;
    }
    if (argc != 2)
      throw std::runtime_error("source path required");
    check(detail::lookup_link({0, "[]"}, "missing").is_null(), "confirmed link absence");
    check(!detail::lookup_present({0, "\n"}), "confirmed OVS absence");
    for (int code : {1, 2, 124}) {
      bool rejected = false;
      try {
        detail::lookup_link({code, "[]"}, "missing");
      } catch (const Failure &e) {
        rejected = e.status == 503;
      }
      check(rejected, "failed link lookup is not absence");
      rejected = false;
      try {
        detail::lookup_present({code, ""});
      } catch (const Failure &e) {
        rejected = e.status == 503;
      }
      check(rejected, "failed OVS lookup is not absence");
    }
    Json expected_ns = {{"namespaceInode", "123"}, {"containerId", "owned"}};
    detail::verify_namespace(expected_ns, {{"Id", "owned"}}, 123);
    bool replaced = false;
    try {
      detail::verify_namespace(expected_ns, {{"Id", "owned"}}, 456);
    } catch (const Failure &e) {
      replaced = e.status == 409;
    }
    check(replaced, "replaced namespace rejected");
    char name[] = "/tmp/graphlab-m2-XXXXXX";
    auto path = mkdtemp(name);
    if (!path)
      throw std::runtime_error("mkdtemp");
    std::filesystem::path directory = path;
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    auto topology = fixture(argv[1], directory, "sha256:" + std::string(64, 'd'));
    graphlab::console::Catalog catalog(directory, directory / "artifacts.lock.json");
    auto hash =
        lab_support::validate(topology, graphlab::console::load(directory / "artifacts.lock.json"))
            ->hash;
    auto start = Json{
        {"topologyHash", hash}, {"idempotencyKey", "start-key-0001"}, {"developmentMode", true}};
    client_race(directory, catalog, start,
                std::filesystem::absolute(argv[0]).parent_path() / "lab");
    auto state = directory / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    Simulated backend;
    {
      Engine engine(state, backend, catalog);
      bool locked = false;
      try {
        Engine other(state, backend, catalog);
      } catch (const Failure &) {
        locked = true;
      }
      check(locked, "single writer lock");
      auto accepted = call(engine, "start", start);
      auto repeated = call(engine, "start", start);
      check(accepted == repeated, "idempotent admission");
      auto changed = start;
      changed["developmentMode"] = false;
      bool conflict = false;
      try {
        call(engine, "start", changed);
      } catch (const Failure &e) {
        conflict = e.status == 409;
      }
      check(conflict, "same key changed payload conflicts");
      check(wait(engine, accepted)["state"] == "succeeded", "start executes");
      auto run = call(engine, "run", {{"id", accepted["runId"]}});
      check(run["state"] == "ready" && !backend.live.empty(), "ready after resource preparation");
      std::string log_node;
      for (const auto &resource : run["resources"])
        if (resource["kind"] == "container") {
          log_node = resource["logical"];
          break;
        }
      check(!log_node.empty(), "log fixture has a workload");
      auto logs = call(engine, "terminal",
                       {{"runId", run["id"]}, {"operation", "logs"}, {"node", log_node}});
      check(logs["runId"] == run["id"] && logs["node"] == log_node,
            "logs use scoped registered node");
      for (const auto &params : std::vector<Json>{
               {{"runId", run["id"]}, {"operation", "logs"}, {"node", "../../etc/passwd"}},
               {{"runId", "foreign-run"}, {"operation", "logs"}, {"node", log_node}},
               {{"runId", run["id"]},
                {"operation", "logs"},
                {"node", log_node},
                {"params", {{"path", "/etc/passwd"}}}}}) {
        bool refused = false;
        try {
          call(engine, "terminal", params);
        } catch (const Failure &) {
          refused = true;
        }
        check(refused, "logs reject foreign identity and arbitrary paths");
      }
      auto op = Json{{"runId", run["id"]},
                     {"operation", "stop"},
                     {"expectedRevision", "0"},
                     {"idempotencyKey", "stop-key-0001"}};
      conflict = false;
      try {
        call(engine, "operate", op);
      } catch (const Failure &e) {
        conflict = e.status == 409;
      }
      check(conflict, "stale revision conflicts");
      op["expectedRevision"] = run["revision"];
      auto stopped = call(engine, "operate", op);
      check(wait(engine, stopped)["state"] == "succeeded", "stop job");
      op["operation"] = "resume";
      op["idempotencyKey"] = "resume-key-0001";
      op["expectedRevision"] = stopped["revision"];
      auto resumed = call(engine, "operate", op);
      check(wait(engine, resumed)["state"] == "succeeded", "resume job");
      op["operation"] = "destroy";
      op["idempotencyKey"] = "destroy-key-0001";
      op["expectedRevision"] = resumed["revision"];
      backend.fail_remove = true;
      check(wait(engine, call(engine, "operate", op))["state"] == "failed",
            "cleanup lookup failure fails job");
      auto uncertain = call(engine, "run", {{"id", run["id"]}});
      check(uncertain["state"] == "reconciling" && !backend.live.empty(),
            "failed cleanup retains uncertain resources");
      check(uncertain["resources"].back()["state"] == "prepared",
            "failed removal is not journaled as removed");
      backend.fail_remove = false;
      op["operation"] = "recover";
      op["idempotencyKey"] = "recover-lookup-0001";
      op["expectedRevision"] = uncertain["revision"];
      check(wait(engine, call(engine, "operate", op))["state"] == "succeeded",
            "cleanup retry succeeds");
      check(backend.live.empty(), "all resources removed");
    }
    {
      Engine engine(state, backend, catalog);
      check(call(engine, "start", start)["revision"] == "1", "idempotency survives restart");
    }
    auto count = resources({{"id", "fixture"}, {"topology", topology}}).size();
    {
      auto cancellation = directory / "cancellation";
      std::filesystem::create_directory(cancellation);
      chmod(cancellation.c_str(), 0700);
      Simulated slow;
      slow.delay_ms = 100;
      Engine engine(cancellation, slow, catalog);
      std::vector<Json> accepted(8);
      std::vector<std::thread> callers;
      for (std::size_t i = 0; i < accepted.size(); ++i)
        callers.emplace_back([&, i] { accepted[i] = call(engine, "start", start); });
      for (auto &caller : callers)
        caller.join();
      for (const auto &a : accepted)
        check(a == accepted[0], "concurrent retries share one job");
      call(engine, "cancel", {{"id", accepted[0]["jobId"]}});
      check(wait(engine, accepted[0])["state"] == "cancelled" && slow.live.empty(),
            "cancellation compensates partial start");
    }
    for (std::size_t boundary = 0; boundary < count; ++boundary)
      for (bool after : {false, true}) {
        auto isolated =
            directory / ("fault-" + std::to_string(boundary) + (after ? "-after" : "-before"));
        std::filesystem::create_directory(isolated);
        chmod(isolated.c_str(), 0700);
        Simulated fault;
        fault.fail = static_cast<int>(boundary);
        fault.after = after;
        Json run;
        {
          Engine engine(isolated, fault, catalog);
          auto accepted = call(engine, "start", start);
          check(wait(engine, accepted)["state"] == "failed", "injected resource boundary fails");
          run = call(engine, "run", {{"id", accepted["runId"]}});
          check(run["state"] == "reconciling", "uncertain state explicit");
        }
        {
          Engine engine(isolated, fault, catalog);
          auto accepted = call(engine, "operate",
                               {{"runId", run["id"]},
                                {"operation", "recover"},
                                {"expectedRevision", run["revision"]},
                                {"idempotencyKey", "recover-key-0001"}});
          check(wait(engine, accepted)["state"] == "succeeded" && fault.live.empty(),
                "restart cleans intended resources");
        }
      }
    std::cout << "M2 journal and boundary tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
