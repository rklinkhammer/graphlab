#include <atomic>
#include <fstream>
#include <graphlab/telemetry.hpp>
#include <iostream>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
Json call(runtime::Engine &e, std::string m, Json p) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, getuid());
}
Json wait(runtime::Engine &e, Json a) {
  for (int i = 0; i < 1000; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("job timeout");
}
struct Fake : runtime::Backend {
  std::atomic<int> applied = 0, removed = 0, delay = 0;
  void preflight(const Json &, const Json &) override {}
  Json prepare(const Json &, const Json &) override { return Json::object(); }
  void remove(const Json &, const Json &) override {}
  void activate(const Json &) override {}
  void gate(const Json &, const std::string &) override {}
  Json observe(const Json &) override { return Json::object(); }
  Json fault(const Json &, const Json &, const std::string &op) override {
    if (op == "apply") {
      ++applied;
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    if (op == "remove")
      ++removed;
    return {{"hook", "egress"}};
  }
};
int main(int argc, char **argv) {
  try {
    Json raw;
    for (auto k : {"rxBytes", "txBytes", "rxPackets", "txPackets", "rxErrors", "txErrors",
                   "rxDropped", "txDropped"})
      raw[k] = "0";
    Json s = {{"runId", "r"},          {"edge", "e"},           {"mappingEpoch", "m"},
              {"collectorEpoch", "c"}, {"bootId", "b"},         {"monotonicNs", "1000000000"},
              {"valid", true},         {"forwardMetric", "rx"}, {"raw", raw}};
    auto first = lab_support::telemetry::derive(nullptr, s);
    check(first["forwardBitsPerSecond"].is_null(), "first sample is unknown");
    s["monotonicNs"] = "2000000000";
    s["raw"]["rxBytes"] = "125";
    auto next = lab_support::telemetry::derive(first, s);
    check(next["forwardBitsPerSecond"] == 1000 && next["reverseBitsPerSecond"] == 0,
          "directional delta without endpoint sum");
    s["monotonicNs"] = "3000000000";
    s["raw"]["rxBytes"] = "1";
    auto reset = lab_support::telemetry::derive(next, s);
    check(reset["gapReason"] == "counter_reset" && reset["forwardBitsPerSecond"].is_null(),
          "counter decrease is gap not spike");
    s["monotonicNs"] = "7000000000";
    check(lab_support::telemetry::derive(reset, s)["gapReason"] == "sample_gap",
          "collector outage creates gap");
    s["mappingEpoch"] = "other";
    check(lab_support::telemetry::derive(reset, s)["gapReason"] == "identity_changed",
          "mapping change resets epoch");
    s["raw"]["rxBytes"] = "18446744073709551600";
    auto large = lab_support::telemetry::derive(nullptr, s);
    s["raw"]["rxBytes"] = "18446744073709551610";
    s["monotonicNs"] = "8000000000";
    check(lab_support::telemetry::derive(large, s)["forwardBitsPerSecond"] == 80,
          "64-bit strings preserve small deltas beyond JS precision");
    sqlite3 *db = nullptr;
    sqlite3_open(":memory:", &db);
    telemetry::initialize(db);
    telemetry::append(db, next);
    telemetry::append(db, reset);
    auto history = telemetry::query(db, "r", {{"resolutionSeconds", 10}});
    check(history["items"][0]["sampleCount"] == 2 && history["items"][0]["gapCount"] == 1,
          "aggregate retains gap and valid sample counts");
    check(!history["oldestAvailableUnixSeconds"].is_null(), "history exposes retention boundary");
    bool range_rejected = false;
    try {
      telemetry::query(db, "r", {{"limit", 2001}});
    } catch (...) {
      range_rejected = true;
    }
    check(range_rejected, "history point budget enforced");
    sqlite3_close(db);
    bool bad = false;
    try {
      telemetry::validate_fault({{"kind", "netem"},
                                 {"edge", "e"},
                                 {"direction", "a-to-b"},
                                 {"durationSeconds", 1},
                                 {"lossPercent", 101}});
    } catch (...) {
      bad = true;
    }
    check(bad, "fault bounds enforced");
    if (argc != 2)
      return 2;
    char dir[] = "/tmp/gl5-unit-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(dir));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};
    auto lock = console::load(std::filesystem::path(argv[1]) / "topologies/artifacts.lock.json");
    auto t = console::load(std::filesystem::path(argv[1]) / "topologies/isolated.yaml");
    t["capture"]["required"] = false;
    std::ofstream(root / "artifacts.lock.json") << lock.dump();
    std::ofstream(root / "topology.yaml") << t.dump();
    console::Catalog catalog(root, root / "artifacts.lock.json");
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    Fake fake;
    Json run;
    std::string faultid;
    {
      runtime::Engine e(state, fake, catalog);
      auto a = call(e, "start",
                    {{"topologyHash", lab_support::validate(t, lock)->hash},
                     {"idempotencyKey", "m5-start"},
                     {"developmentMode", true}});
      check(wait(e, a)["state"] == "succeeded", "fixture run starts");
      run = call(e, "run", {{"id", a["runId"]}});
      Json p = {{"runId", run["id"]},
                {"expectedRevision", run["revision"]},
                {"idempotencyKey", "m5-fault-one"},
                {"fault",
                 {{"kind", "netem"},
                  {"edge", "e"},
                  {"direction", "a-to-b"},
                  {"delayMs", 100},
                  {"durationSeconds", 1}}}};
      auto f = call(e, "fault.apply", p);
      check(call(e, "fault.apply", p) == f, "idempotent fault retry shares job");
      check(wait(e, f)["state"] == "succeeded", "fault applies");
      faultid = f["faultId"];
      bad = false;
      try {
        p["idempotencyKey"] = "m5-stale-key";
        call(e, "fault.apply", p);
      } catch (...) {
        bad = true;
      }
      check(bad, "stale revision rejected");
      p["expectedRevision"] = call(e, "run", {{"id", run["id"]}})["revision"];
      bad = false;
      try {
        call(e, "fault.apply", p);
      } catch (const std::exception &ex) {
        bad = std::string(ex.what()) == "fault_direction_busy";
      }
      check(bad, "overlapping same-direction fault conflicts");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    {
      runtime::Engine e(state, fake, catalog);
      run = call(e, "run", {{"id", run["id"]}});
      check(run["faults"][faultid]["state"] == "removed" && fake.removed > 0,
            "restart removes expired fault before new operations");
      fake.delay = 500;
      auto before_apply = fake.applied.load();
      auto before_remove = fake.removed.load();
      Json p = {{"runId", run["id"]},
                {"expectedRevision", run["revision"]},
                {"idempotencyKey", "m5-cancel-fault"},
                {"fault",
                 {{"kind", "netem"},
                  {"edge", "e"},
                  {"direction", "a-to-b"},
                  {"lossPercent", 100},
                  {"durationSeconds", 30}}}};
      auto f = call(e, "fault.apply", p);
      for (int i = 0; i < 100 && fake.applied == before_apply; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      check(fake.applied > before_apply, "cancellation targets an executing fault");
      call(e, "cancel", {{"id", f["jobId"]}});
      check(wait(e, f)["state"] == "cancelled" && fake.removed > before_remove,
            "running fault cancellation compensates the applied qdisc");
      run = call(e, "run", {{"id", run["id"]}});
      auto end = call(e, "operate",
                      {{"runId", run["id"]},
                       {"expectedRevision", run["revision"]},
                       {"idempotencyKey", "m5-cleanup"},
                       {"operation", "recover"}});
      check(wait(e, end)["state"] == "succeeded", "fault-aware cleanup");
    }
    std::cout << "M5 portable tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
