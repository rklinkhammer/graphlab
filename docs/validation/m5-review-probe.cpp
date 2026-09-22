// Standalone review probe; reuses the existing portable fixture helpers.
#define main m5_original_main
#include "../../tests/telemetry/m5.cpp"
#undef main
struct ExpiryFailure : Fake {
  std::atomic<int> quiesces = 0;
  bool fail_remove = true;
  void gate(const Json &, const std::string &action) override {
    if (action == "quiesce")
      ++quiesces;
  }
  Json fault(const Json &r, const Json &f, const std::string &op) override {
    if (op == "remove" && fail_remove)
      throw runtime::Failure("injected_remove_failure");
    return Fake::fault(r, f, op);
  }
};
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    sqlite3 *db = nullptr;
    sqlite3_open(":memory:", &db);
    telemetry::initialize(db);
    bool rejected = false;
    try {
      telemetry::query(db, "r",
                       {{"resolutionSeconds", 1},
                        {"fromUnixSeconds", std::to_string(telemetry::wall() - 3601)}});
    } catch (const std::exception &ex) {
      rejected = std::string(ex.what()) == "invalid_telemetry_range";
    }
    check(rejected, "REPRO: last-hour query from a browser one second behind is rejected");
    sqlite3_close(db);
    char dir[] = "/tmp/gl5-review-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(dir));
    auto artifacts =
        console::load(std::filesystem::path(argv[1]) / "topologies/artifacts.lock.json");
    auto t = console::load(std::filesystem::path(argv[1]) / "topologies/isolated.yaml");
    t["capture"]["required"] = false;
    std::ofstream(root / "artifacts.lock.json") << artifacts.dump();
    std::ofstream(root / "topology.yaml") << t.dump();
    console::Catalog catalog(root, root / "artifacts.lock.json");
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    ExpiryFailure backend;
    {
      runtime::Engine engine(state, backend, catalog);
      auto start = call(engine, "start",
                        {{"topologyHash", lab_support::validate(t, artifacts)->hash},
                         {"idempotencyKey", "review-start"},
                         {"developmentMode", true}});
      check(wait(engine, start)["state"] == "succeeded", "probe fixture starts");
      auto run = call(engine, "run", {{"id", start["runId"]}});
      auto apply = call(engine, "fault.apply",
                        {{"runId", run["id"]},
                         {"expectedRevision", run["revision"]},
                         {"idempotencyKey", "review-fault"},
                         {"fault",
                          {{"kind", "netem"},
                           {"edge", "fixture"},
                           {"direction", "a-to-b"},
                           {"delayMs", 100},
                           {"durationSeconds", 1}}}});
      check(wait(engine, apply)["state"] == "succeeded", "probe fault applies");
      const int before = backend.quiesces;
      for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        run = call(engine, "run", {{"id", start["runId"]}});
        if (run["state"] == "reconciling")
          break;
      }
      check(run["state"] == "reconciling" && run["faultRecoveryError"] == "injected_remove_failure",
            "expiry removal failure is persisted");
      check(backend.quiesces == before, "REPRO: expiry removal failure does not quiesce traffic");
      std::cout << "fault state: " << run["faults"][apply["faultId"].get<std::string>()]["state"]
                << '\n';
      backend.fail_remove = false;
      check(wait(engine, call(engine, "operate",
                              {{"runId", run["id"]},
                               {"expectedRevision", run["revision"]},
                               {"idempotencyKey", "review-cleanup"},
                               {"operation", "recover"}}))["state"] == "succeeded",
            "probe cleanup");
    }
    std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
