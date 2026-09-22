// Regression coverage for M5 expiry failures and server-relative history.
#define main m5_original_main
#include "m5.cpp"
#undef main
struct ExpiryFailure : Fake {
  std::atomic<int> quiesces = 0;
  std::atomic<bool> fail_remove = true, fail_quiesce = false;
  void gate(const Json &, const std::string &action) override {
    if (action == "quiesce")
      ++quiesces;
    if (action == "quiesce" && fail_quiesce)
      throw runtime::Failure("injected_quiesce_failure");
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
    check(rejected, "absolute ranges exceeding the budget remain rejected");
    for (auto [resolution, window] : {std::pair{1, 3600}, {10, 86400}, {60, 604800}}) {
      const auto h =
          telemetry::query(db, "r", {{"resolutionSeconds", resolution}, {"windowSeconds", window}});
      check(std::stoll(h["toUnixSeconds"].get<std::string>()) -
                    std::stoll(h["fromUnixSeconds"].get<std::string>()) ==
                window,
            "relative window uses one server timestamp");
    }
    for (auto p : {Json{{"windowSeconds", 3601}}, Json{{"windowSeconds", 0}},
                   Json{{"windowSeconds", 3600}, {"fromUnixSeconds", "1"}}}) {
      bool bad = false;
      try {
        telemetry::query(db, "r", p);
      } catch (...) {
        bad = true;
      }
      check(bad, "invalid or mixed relative range rejected");
    }
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
      std::ofstream(root / "run-id.json") << Json{{"id", run["id"]}}.dump();
      const int before = backend.quiesces;
      for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        run = call(engine, "run", {{"id", start["runId"]}});
        if (run["state"] == "reconciling" && !run["quiescenceAcknowledgedAt"].is_null())
          break;
      }
      check(run["state"] == "reconciling" && run["faultRecoveryError"] == "injected_remove_failure",
            "expiry removal failure is persisted");
      check(backend.quiesces > before && !run["quiescenceAcknowledgedAt"].is_null(),
            "expiry failure quiesces a development run without a lease");
      check(run["faults"][apply["faultId"].get<std::string>()]["state"] == "recovery-required",
            "uncertain fault retains explicit recovery state");
      std::cout << "fault state: " << run["faults"][apply["faultId"].get<std::string>()]["state"]
                << '\n';
      check(std::any_of(run["timeline"].begin(), run["timeline"].end(),
                        [](const Json &v) { return v["kind"] == "fault-quiescence"; }),
            "quiescence is correlated in timeline");
    }
    backend.fail_quiesce = true;
    {
      runtime::Engine engine(state, backend, catalog);
      auto saved = console::load(root / "run-id.json");
      auto run = call(engine, "run", {{"id", saved["id"]}});
      check(run["state"] == "reconciling" && run["quiescenceAcknowledgedAt"].is_null() &&
                run["quiescenceError"] == "injected_quiesce_failure",
            "restart removal and quiescence failures remain explicit");
      backend.fail_quiesce = false;
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
