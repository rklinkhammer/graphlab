// Explicit two-stage host reboot qualification, never part of ordinary CTest.
#define main m5_fixture_main
#include "../telemetry/linux.cpp"
#undef main
#include <graphlab/terminal.hpp>
int main(int argc, char **argv) {
  if (argc != 5 || geteuid()) {
    std::cerr << "Usage (root): m6_reboot prepare|verify SOURCE IMAGE_ID STATE_ROOT\n";
    return 2;
  }
  int lockfd = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB))
    return 2;
  try {
    std::string mode = argv[1];
    auto root = std::filesystem::absolute(argv[4]);
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    if (mode == "prepare") {
      if (std::filesystem::exists(root))
        throw std::runtime_error("state root already exists");
      std::filesystem::create_directories(root);
      chmod(root.c_str(), 0700);
      check(runtime::process(
                {(bin / "m2_tests").string(), "--fixtures", argv[2], root.string(), argv[3]})
                    .code == 0,
            "reboot fixture");
      auto t = console::load(root / "m2.yaml");
      t["capture"]["required"] = true;
      std::ofstream(root / "m2.yaml") << t.dump();
      std::filesystem::create_directory(root / "state");
      chmod((root / "state").c_str(), 0700);
    } else if (mode != "verify")
      throw std::runtime_error("unknown reboot stage");
    console::Catalog catalog(root, root / "artifacts.lock.json");
    runtime::LinuxBackend backend;
    auto before = mode == "verify" ? console::load(root / "before.json") : Json::object();
    if (mode == "verify")
      check(before["hostBootId"] != telemetry::boot(), "host boot identity changed");
    runtime::Engine engine(root / "state", backend, catalog);
    if (mode == "prepare") {
      auto valid = lab_support::validate(console::load(root / "m2.yaml"),
                                         console::load(root / "artifacts.lock.json"));
      auto a = call(engine, "start",
                    {{"topologyHash", valid->hash},
                     {"idempotencyKey", "m6-reboot-start"},
                     {"capturePolicy",
                      {{"runBytes", 32 * 1024 * 1024},
                       {"reserveBytes", 1024 * 1024},
                       {"rotateBytes", 1024 * 1024},
                       {"rotateSeconds", 2}}}});
      check(wait(engine, a)["state"] == "succeeded", "reboot capture run starts");
      auto run = call(engine, "run", {{"id", a["runId"]}});
      auto session = call(engine, "terminal",
                          {{"runId", run["id"]},
                           {"node", "a"},
                           {"sessionId", ""},
                           {"operation", "open"},
                           {"owner", "m6-reboot"},
                           {"params", Json::object()}});
      run = call(engine, "run", {{"id", a["runId"]}});
      run["hostBootId"] = telemetry::boot();
      std::ofstream(root / "before.json") << run.dump(2);
      check(!run["captures"].empty() && !run["sessions"].empty(),
            "capture and terminal descriptors retained");
      std::cout << "Prepared for explicit host reboot: " << root << std::endl;
    } else {
      auto run = call(engine, "run", {{"id", before["id"]}});
      std::ofstream(root / "after.json") << run.dump(2);
      check(run["state"] == "reconciling" && run["captureCoverage"] == "incomplete",
            "prior-boot run cannot claim continuity");
      check(std::stoull(run["controllerGeneration"].get<std::string>()) >
                std::stoull(before["controllerGeneration"].get<std::string>()),
            "new controller generation");
      bool rejected = false;
      try {
        backend.capture_control(before, "status");
      } catch (...) {
        rejected = true;
      }
      check(rejected, "prior-boot capture authority rejected");
      rejected = false;
      try {
        terminal::request(before["sessions"][0], "status", before["controllerGeneration"]);
      } catch (...) {
        rejected = true;
      }
      check(rejected, "old terminal handle cannot reattach to unrelated session");
      auto j = wait(engine, call(engine, "operate",
                                 {{"runId", run["id"]},
                                  {"expectedRevision", run["revision"]},
                                  {"operation", "recover"},
                                  {"idempotencyKey",
                                   "m6-reboot-cleanup-" + run["revision"].get<std::string>()}}));
      std::ofstream(root / "cleanup.json") << j.dump(2);
      check(j["state"] == "succeeded", "prior-boot cleanup");
      auto artifacts = call(engine, "artifacts", {{"runId", run["id"]}});
      std::ofstream(root / "artifacts.json") << artifacts.dump(2);
      check(!artifacts.at("items").empty(), "retained artifacts indexed after reboot");
      for (const auto &capture : before["captures"]) {
        bool found = false;
        for (const auto &artifact : artifacts["items"])
          if (artifact.value("captureId", Json(nullptr)) == capture["id"] &&
              artifact.value("state", "") != "unavailable-or-partial")
            found = true;
        check(found, "prior-boot capture has a retained readable segment");
      }
      std::cout << "M6 reboot checks passed\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
