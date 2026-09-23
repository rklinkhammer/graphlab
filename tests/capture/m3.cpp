#include <atomic>
#include <fstream>
#include <graphlab/capture.hpp>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <pcap/pcap.h>
#endif
using namespace graphlab::runtime;
namespace {
void check(bool b, const std::string &message) {
  if (!b)
    throw std::runtime_error(message);
  std::cout << "PASS " << message << '\n';
}
Json call(Engine &e, std::string m, Json p = Json::object()) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(Engine &e, const Json &a) {
  for (int i = 0; i < 1000; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("timeout");
}
struct Fake : Backend {
  bool fail_arm = false;
  bool interrupted_stop = false;
  std::atomic<bool> unhealthy = false;
  std::atomic<int> released = 0, held = 0, armed = 0, adopted = 0, closed = 0;
  void preflight(const Json &, const Json &) override {}
  Json prepare(const Json &, const Json &r) override { return {{"id", r["key"]}}; }
  void remove(const Json &, const Json &) override {}
  void activate(const Json &) override { check(armed > 0, "captures armed before convergence"); }
  void gate(const Json &, const std::string &a) override {
    if (a == "release") {
      check(armed > 0, "capture barrier before release");
      ++released;
    }
    if (a == "quiesce")
      ++held;
  }
  Json observe(const Json &) override { return {{"observedAt", graphlab::console::timestamp()}}; }
  Json capture_plan(const Json &r) override {
    Json j = Json::array();
    for (const auto &e : r["topology"]["edges"])
      j.push_back({{"id", e["id"]}});
    return j;
  }
  Json capture_control(const Json &, const std::string &a) override {
    if (a == "arm") {
      if (fail_arm)
        throw Failure("injected_arm_failure");
      ++armed;
    }
    if (a == "status" && unhealthy)
      throw Failure("injected_capture_loss");
    if (a == "adopt")
      ++adopted;
    if (a == "stop") {
      ++closed;
      return Json::array({{{"state", interrupted_stop ? "interrupted" : "closed"}}});
    }
    return Json::array();
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("source and fixture generator required");
    char pattern[] = "/tmp/gl-m3-XXXXXX";
    auto dir = std::filesystem::path(mkdtemp(pattern));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir};
    auto r =
        process({argv[2], "--fixtures", argv[1], dir.string(), "sha256:" + std::string(64, 'd')});
    check(r.code == 0, "capture fixture generated");
    auto topology = graphlab::console::load(dir / "m2.yaml");
    topology["capture"]["required"] = true;
    std::ofstream(dir / "m2.yaml") << topology.dump();
    graphlab::console::Catalog catalog(dir, dir / "artifacts.lock.json");
    auto hash =
        lab_support::validate(topology, graphlab::console::load(dir / "artifacts.lock.json"))->hash;
    auto admission = Json{{"topologyHash", hash}, {"idempotencyKey", "m3-start-key"}};
    for (const auto profile : {"full", "minimal"})
      for (bool failure : {true, false}) {
        auto state = dir / (std::string(profile) + (failure ? "-failed" : "-healthy"));
        std::filesystem::create_directory(state);
        chmod(state.c_str(), 0700);
        Fake backend;
        backend.fail_arm = failure;
        Json run;
        {
          Engine engine(state, backend, catalog);
          auto profiled = admission;
          profiled["observationProfile"] = profile;
          auto a = call(engine, "start", profiled);
          auto j = wait(engine, a);
          run = call(engine, "run", {{"id", a["runId"]}});
          if (failure) {
            check(j["state"] == "failed" && backend.released == 0,
                  "failed arm never releases traffic");
            continue;
          }
          check(j["state"] == "succeeded" && run["captureCoverage"] == "recording",
                "capture-first run succeeds");
          check(run["captures"].size() == topology["edges"].size(),
                "one capture per declared edge");
          auto op = [&](std::string action, std::string key) {
            run = call(engine, "run", {{"id", run["id"]}});
            return wait(engine, call(engine, "operate",
                                     {{"runId", run["id"]},
                                      {"expectedRevision", run["revision"]},
                                      {"operation", action},
                                      {"idempotencyKey", key}}));
          };
          check(op("stop", "m3-stop-key")["state"] == "succeeded" && backend.closed > 0,
                "quiesce closes captures");
          check(op("resume", "m3-resume-key")["state"] == "succeeded",
                "resume rearms capture barrier");
          run = call(engine, "run", {{"id", run["id"]}});
          check(run["captureEpoch"] == "2", "resume creates new capture epoch");
          backend.unhealthy = true;
          for (int i = 0; i < 400; ++i) {
            run = call(engine, "run", {{"id", run["id"]}});
            if (run["state"] == "reconciling")
              break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          check(run["captureCoverage"] == "incomplete" && backend.held > 0,
                "capture loss marks incomplete and quiesces");
        }
        {
          Engine recovered(state, backend, catalog);
          check(backend.adopted == 1, "restart adopts surviving captures");
          run = call(recovered, "run", {{"id", run["id"]}});
          check(run["state"] == "reconciling" && backend.released == 2,
                "adoption does not resume stale traffic");
          check(run["captureCoverage"] == "incomplete", "restart preserves known coverage loss");
        }
      }
    auto stopped_state = dir / "stopped";
    std::filesystem::create_directory(stopped_state);
    chmod(stopped_state.c_str(), 0700);
    Fake stopped_backend;
    Json stopped_run;
    {
      Engine engine(stopped_state, stopped_backend, catalog);
      auto a = call(engine, "start", admission);
      check(wait(engine, a)["state"] == "succeeded", "stopped restart fixture starts");
      stopped_run = call(engine, "run", {{"id", a["runId"]}});
      check(wait(engine, call(engine, "operate",
                              {{"runId", stopped_run["id"]},
                               {"expectedRevision", stopped_run["revision"]},
                               {"operation", "stop"},
                               {"idempotencyKey", "stopped-restart"}}))["state"] == "succeeded",
            "stopped restart fixture closes captures");
    }
    {
      Engine engine(stopped_state, stopped_backend, catalog);
      auto run = call(engine, "run", {{"id", stopped_run["id"]}});
      check(run["state"] == "stopped" && run["captureCoverage"] == "closed" &&
                stopped_backend.adopted == 0 && stopped_backend.released == 1,
            "restart preserves stopped run without adopting closed workers");
    }
    auto interrupted_state = dir / "interrupted-stop";
    std::filesystem::create_directory(interrupted_state);
    chmod(interrupted_state.c_str(), 0700);
    Fake interrupted_backend;
    Json interrupted_run;
    {
      Engine engine(interrupted_state, interrupted_backend, catalog);
      auto a = call(engine, "start", admission);
      check(wait(engine, a)["state"] == "succeeded", "interrupted stop fixture starts");
      interrupted_run = call(engine, "run", {{"id", a["runId"]}});
      auto operate = [&](const std::string &action, const std::string &key) {
        auto r = call(engine, "run", {{"id", a["runId"]}});
        return wait(engine, call(engine, "operate",
                                 {{"runId", r["id"]},
                                  {"expectedRevision", r["revision"]},
                                  {"operation", action},
                                  {"idempotencyKey", key}}));
      };
      interrupted_backend.interrupted_stop = true;
      check(operate("stop", "interrupted-stop")["state"] == "succeeded",
            "interrupted worker can be stopped");
      auto r = call(engine, "run", {{"id", a["runId"]}});
      check(r["captureCoverage"] == "closed-incomplete" &&
                r["captureObservations"][0]["state"] == "interrupted",
            "stop persists interrupted observations and incomplete coverage");
      interrupted_backend.interrupted_stop = false;
      for (const auto &action : {"stop", "destroy", "destroy"}) {
        check(operate(action,
                      "repeat-" + std::to_string(interrupted_backend.closed.load()))["state"] ==
                  "succeeded",
              "repeat stop or cleanup succeeds");
        check(call(engine, "run", {{"id", a["runId"]}})["captureCoverage"] == "closed-incomplete",
              "later closed observations never erase a known gap");
      }
    }
    auto rejected_dir = dir / "restart-only";
    std::filesystem::create_directory(rejected_dir);
    chmod(rejected_dir.c_str(), 0700);
    auto rejected_lock = graphlab::console::load(dir / "artifacts.lock.json");
    auto &entry = rejected_lock["workloads"]["app-a"];
    entry["contract"]["lifecycle"]["quiesce"] = "restart-required";
    entry["contractSha256"] = lab_support::digest(entry["contract"]);
    auto rejected_topology = topology;
    rejected_topology["artifactLock"] = lab_support::digest(rejected_lock);
    std::ofstream(rejected_dir / "artifacts.lock.json") << rejected_lock.dump();
    std::ofstream(rejected_dir / "topology.yaml") << rejected_topology.dump();
    graphlab::console::Catalog rejected_catalog(rejected_dir, rejected_dir / "artifacts.lock.json");
    Fake rejected_backend;
    {
      Engine rejected_engine(rejected_dir, rejected_backend, rejected_catalog);
      auto request = admission;
      request["topologyHash"] = lab_support::validate(rejected_topology, rejected_lock)->hash;
      bool denied = false;
      try {
        call(rejected_engine, "start", request);
      } catch (const Failure &e) {
        denied = std::string(e.what()) == "capture_requires_reversible_quiescence";
      }
      check(denied && call(rejected_engine, "runs")["items"].empty(),
            "restart-only workload rejected before admission");
    }
    auto output = dir / "pcap";
    std::filesystem::create_directory(output);
    Json config = {
        {"snaplen", 65535}, {"interface", "test0"}, {"edge", "test-edge"}, {"byteBudget", 1048576}};
    graphlab::capture::Writer writer(output, config);
    writer.open();
    std::vector<unsigned char> packet(60, 0);
    packet[12] = 8;
    packet[13] = 6;
    writer.packet(1700000000123456, packet, 60);
    writer.close({{"received", 1}, {"dropped", nullptr}});
    check(writer.segments().size() == 1 && writer.segments()[0]["state"] == "closed",
          "closed capture manifest");
    check(writer.segments()[0]["statistics"]["dropped"].is_null(), "unknown drops remain null");
    writer.open();
    writer.packet(1700000001123456, packet, 60);
    writer.close({{"received", 2}, {"dropped", 0}});
    check(writer.segments().size() == 2, "rotation preserves both segments");
    auto original = std::filesystem::file_size(output / "0.pcapng");
    {
      std::ofstream tail(output / "0.pcapng", std::ios::binary | std::ios::app);
      tail << "broken";
    }
    auto partial = graphlab::capture::inspect_partial(output / "0.pcapng");
    check(partial["validPrefixBytes"] == std::to_string(original) &&
              std::filesystem::file_size(output / "0.pcapng") == original + 6,
          "partial validation preserves interrupted bytes");
    std::filesystem::resize_file(output / "0.pcapng", original);
    auto original_hash = graphlab::capture::file_hash(output / "0.pcapng");
    bool collision = false;
    try {
      graphlab::capture::Writer duplicate(output, config);
      duplicate.open();
      duplicate.packet(0, packet, 60);
      duplicate.close(Json::object());
    } catch (const Failure &) {
      collision = true;
    }
    check(collision && graphlab::capture::file_hash(output / "0.pcapng") == original_hash,
          "reused directory cannot overwrite closed segment");
#ifdef __linux__
    char error[PCAP_ERRBUF_SIZE]{};
    auto p = pcap_open_offline((output / "0.pcapng").c_str(), error);
    check(p != nullptr, "independent libpcap opens PCAPNG");
    pcap_pkthdr *h;
    const unsigned char *bytes;
    check(pcap_next_ex(p, &h, &bytes) == 1 && h->len == 60 && h->caplen == 60 &&
              h->ts.tv_usec == 123456,
          "independent reader verifies packet length and timestamp");
    pcap_close(p);
#endif
    auto limited = dir / "quota";
    std::filesystem::create_directory(limited);
    config["byteBudget"] = 256;
    bool exhausted = false;
    {
      graphlab::capture::Writer quota(limited, config);
      quota.open();
      try {
        for (int i = 0; i < 10; ++i)
          quota.packet(0, packet, 60);
      } catch (const Failure &) {
        exhausted = true;
      }
    }
    check(exhausted && std::filesystem::exists(limited / "0.pcapng.partial") &&
              !std::filesystem::exists(limited / "0.pcapng"),
          "quota preserves partial file without false completion");
    std::cout << "M3 tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
