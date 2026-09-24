#include "../packages/lab-support/cpp/source_control.hpp"
#include <atomic>
#include <fstream>
#include <graphlab/runtime.hpp>
#include <iostream>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab::runtime;
void check(bool v, const char *s) {
  if (!v)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << '\n';
}
template <class F> void rejects(F f, const char *s) {
  bool bad = false;
  try {
    f();
  } catch (const std::exception &) {
    bad = true;
  }
  check(bad, s);
}
Json call(Engine &e, std::string m, Json p) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(Engine &e, Json p) {
  for (int i = 0; i < 500; ++i) {
    auto q = call(e, "source-controls.query", {{"runId", p["runId"]}, {"node", p["node"]}});
    for (auto c : q["commands"])
      if (c["request"]["requestId"] == p["requestId"] && c["outcome"] != "requested")
        return c;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("wait_timeout");
}
struct Fake : Backend {
  lab_support::detail::Sources sources{std::string(32, 'a')};
  std::atomic<bool> timeout = false, bad_ack = false, block = false;
  std::atomic<int> sends = 0;
  void preflight(const Json &, const Json &) override {}
  Json prepare(const Json &, const Json &) override { return {{"id", std::string(64, 'b')}}; }
  void remove(const Json &, const Json &) override {}
  void activate(const Json &) override {}
  void gate(const Json &, const std::string &) override {}
  Json observe(const Json &) override { return Json::object(); }
  Json source_control(const Json &, const Json &r, const Json &p) override {
    if (r["logical"] != "b")
      throw Failure("source_control_unsupported");
    if (p.is_null())
      return sources.status({0, 0});
    ++sends;
    for (int i = 0; block && i < 300; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (timeout)
      throw Failure("source_timeout");
    if (bad_ack)
      return Json::object();
    return sources.apply(p);
  }
};
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("source required");
    lab_support::detail::Sources s(std::string(32, 'a'));
    Json p = {{"apiVersion", "graphlab.source-control/v1"},
              {"epoch", std::string(32, 'a')},
              {"source", "alpha"},
              {"action", "pause"},
              {"requestId", "one"}};
    check(s.apply(p)["outcome"] == "acknowledged" && s.paused(0) && !s.paused(1),
          "pause affects only selected generator");
    auto resume = p;
    resume["requestId"] = "two";
    resume["action"] = "resume";
    s.apply(resume);
    s.apply(p);
    check(!s.paused(0), "old duplicate acknowledgement cannot reapply pause");
    auto bad = p;
    bad["action"] = "resume";
    rejects([&] { s.apply(bad); }, "conflicting ID rejected");
    bad = p;
    bad["epoch"] = std::string(32, 'c');
    rejects([&] { s.apply(bad); }, "old process epoch rejected");
    bad = p;
    bad["extra"] = true;
    rejects([&] { s.apply(bad); }, "unknown fields rejected");
    for (int i = 2; i < 256; ++i) {
      auto c = p;
      c["requestId"] = "id" + std::to_string(i);
      s.apply(c);
    }
    bad = p;
    bad["requestId"] = "overflow";
    rejects([&] { s.apply(bad); }, "workload 256 record bound never evicts idempotency");
    s.apply(p);
    char temp[] = "/tmp/graphlab-source-XXXXXX";
    auto dir = std::filesystem::path(mkdtemp(temp));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir};
    auto lock =
        graphlab::console::load(std::filesystem::path(argv[1]) / "topologies/artifacts.lock.json");
    lock["workloads"].erase("guest-linux");
    for (auto &[id, w] : lock["workloads"].items())
      w["image"] = "graphlab.local/" + id + "@sha256:" + std::string(64, 'd');
    auto t = graphlab::console::load(std::filesystem::path(argv[1]) / "topologies/triangle.yaml");
    t["nodes"].erase("g");
    t["management"] = {{"networks", Json::object()}};
    t["managementAttachments"] = Json::array();
    t["capture"]["required"] = false;
    Json edges = Json::array();
    for (auto edge : t["edges"])
      if (edge["id"] != "g-s")
        edges.push_back(edge);
    t["edges"] = edges;
    t["artifactLock"] = lab_support::digest(lock);
    t["application"] = {{"apiVersion", "graphlab.application-dataflow/v1"},
                        {"edges", Json::array({{{"id", "alpha"},
                                                {"source", "b"},
                                                {"target", "a"},
                                                {"networkEdges", Json::array()}}})}};
    std::ofstream(dir / "topology.yaml") << t.dump();
    std::ofstream(dir / "artifacts.lock.json") << lock.dump();
    graphlab::console::Catalog catalog(dir, dir / "artifacts.lock.json");
    auto validated = lab_support::validate(t, lock);
    if (!validated)
      throw std::runtime_error("fixture_invalid");
    auto state = dir / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    Fake backend;
    std::string run;
    {
      Engine e(state, backend, catalog);
      auto accepted = call(e, "start",
                           {{"topologyHash", validated->hash},
                            {"idempotencyKey", "source-start-0001"},
                            {"developmentMode", true}});
      run = accepted["runId"];
      for (int i = 0; i < 500; ++i) {
        if (call(e, "job", {{"id", accepted["jobId"]}})["state"] == "succeeded")
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      p["runId"] = run;
      p["node"] = "b";
      p["instance"] = std::string(64, 'b');
      p["requestId"] = "agent-one";
      backend.block = true;
      check(call(e, "source-controls.command", p)["outcome"] == "requested",
            "durable requested outcome");
      bad = p;
      bad["requestId"] = "competing-pause";
      rejects([&] { call(e, "source-controls.command", bad); },
              "second pending command for the same source rejected");
      backend.block = false;
      auto done = wait(e, p);
      check(done["outcome"] == "acknowledged", "worker acknowledgement persisted");
      check(call(e, "source-controls.command", p) == done && backend.sends == 1,
            "agent duplicate does not dispatch again");
      bad = p;
      bad["instance"] = std::string(64, 'c');
      rejects([&] { call(e, "source-controls.command", bad); },
              "ID cannot target replacement instance");
      bad["requestId"] = "wrong-instance";
      rejects([&] { call(e, "source-controls.command", bad); },
              "new request wrong instance rejected");
      bad = p;
      bad["epoch"] = std::string(32, 'c');
      bad["requestId"] = "wrong-epoch";
      rejects([&] { call(e, "source-controls.command", bad); },
              "wrong epoch rejected before queuing");
      bad = p;
      bad["node"] = "a";
      bad["requestId"] = "wrong-source";
      rejects([&] { call(e, "source-controls.command", bad); }, "undeclared source owner rejected");
      check(
          call(e, "source-controls.query", {{"runId", run}, {"node", "a"}})["capability"].is_null(),
          "legacy receiver advertises no source control");
      backend.timeout = true;
      p["requestId"] = "timeout";
      call(e, "source-controls.command", p);
      check(wait(e, p)["outcome"] == "timed-out", "transport timeout is uncertain outcome");
      backend.timeout = false;
      backend.bad_ack = true;
      p["requestId"] = "bad-ack";
      call(e, "source-controls.command", p);
      check(wait(e, p)["outcome"] == "failed", "mismatched acknowledgement cannot succeed");
      backend.bad_ack = false;
    }
    // Simulate durable requested record left by process death, with no replay on startup.
    sqlite3 *db = nullptr;
    sqlite3_open((state / "state.sqlite").c_str(), &db);
    sqlite3_stmt *q = nullptr;
    sqlite3_prepare_v2(db, "SELECT document FROM state", -1, &q, nullptr);
    sqlite3_step(q);
    auto doc = Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(q, 0)));
    sqlite3_finalize(q);
    for (int i = 3; i < 256; ++i) {
      auto c = doc["sourceCommands"].begin().value();
      c["request"]["requestId"] = "retained-" + std::to_string(i);
      doc["sourceCommands"]["retained-" + std::to_string(i)] = c;
    }
    for (auto &[id, c] : doc["sourceCommands"].items())
      c["outcome"] = "requested";
    sqlite3_prepare_v2(db, "UPDATE state SET document=?", -1, &q, nullptr);
    auto body = doc.dump();
    sqlite3_bind_text(q, 1, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(q);
    sqlite3_finalize(q);
    sqlite3_close(db);
    {
      Engine e(state, backend, catalog);
      auto q = call(e, "source-controls.query", {{"runId", run}, {"node", "b"}});
      check(q["commands"].size() == 256 &&
                std::all_of(q["commands"].begin(), q["commands"].end(),
                            [](const Json &c) { return c["outcome"] == "interrupted"; }),
            "restart fences bounded retained commands without replay");
      p["requestId"] = "agent-capacity";
      bool capacity = false;
      try {
        call(e, "source-controls.command", p);
      } catch (const Failure &f) {
        capacity = f.status == 429;
      }
      check(capacity, "agent 256-record ledger rejects overflow without eviction");
      check(backend.sends == 3, "restart sends no old commands");
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
