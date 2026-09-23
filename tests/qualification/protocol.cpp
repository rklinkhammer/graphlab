// Real immutable mixed-version nodes; compile once before and once after the 1.1 change.
#define main existing_runtime_main
#include "../runtime/linux_integration.cpp"
#undef main
int main(int argc, char **argv) {
  if (argc != 4 || geteuid())
    return 2; // SOURCE RELEASES_JSON legacy|current
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    auto releases = graphlab::console::load(argv[2]);
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    char tmp[] = "/tmp/gl6-proto-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    std::cout << "Evidence: " << root << std::endl;
    Json proof = Json::array();
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"1.0", "1.0"}, {"1.0", "1.1"}, {"1.1", "1.0"}, {"1.1", "1.1"}};
    if (std::string(argv[3]) == "current")
      pairs.push_back({"1.0", "unsupported"});
    for (const auto &[av, bv] : pairs) {
      auto dir = root / (av + "-" + bv);
      std::filesystem::create_directory(dir);
      check(process({(bin / "m2_tests").string(), "--fixtures", argv[1], dir.string(),
                     releases[av]["app-a"]})
                    .code == 0,
            "independent version fixture");
      auto t = graphlab::console::load(dir / "m2.yaml"),
           artifacts = graphlab::console::load(dir / "artifacts.lock.json");
      for (auto id : {"s1", "s2", "s3"})
        t["nodes"].erase(id);
      for (auto id : {"a", "b"})
        t["nodes"][id]["ports"].erase("data1");
      t["edges"] =
          Json::array({{{"id", "wire"}, {"endpoints", Json::array({"a:data0", "b:data0"})}}});
      t["capture"]["required"] = true;
      for (auto [app, version] :
           {std::pair{std::string("app-a"), av}, std::pair{std::string("app-b"), bv}}) {
        auto entry = releases[version];
        auto &a = artifacts["workloads"][app];
        a["image"] = "graphlab.local/" + app + "@" + entry[app].get<std::string>();
        a["contract"]["labSupport"]["version"] = entry["version"];
        a["contract"]["labSupport"]["packageSha256"] = entry["sdkSha256"];
        a["contractSha256"] = lab_support::digest(a["contract"]);
      }
      t["artifactLock"] = lab_support::digest(artifacts);
      write(dir / "artifacts.lock.json", artifacts);
      write(dir / "m2.yaml", t);
      auto validated = lab_support::validate(t, artifacts);
      check(bool(validated), "pinned package contracts validate");
      // Unsupported workload-contract major must fail without starting any run.
      auto invalid = artifacts;
      invalid["workloads"]["app-a"]["contract"]["apiVersion"] = "graphlab.workload/v99";
      invalid["workloads"]["app-a"]["contractSha256"] =
          lab_support::digest(invalid["workloads"]["app-a"]["contract"]);
      auto bad = t;
      bad["artifactLock"] = lab_support::digest(invalid);
      check(!lab_support::validate(bad, invalid), "unsupported workload contract major rejected");
      graphlab::console::Catalog catalog(dir, dir / "artifacts.lock.json");
      auto state = dir / "state";
      std::filesystem::create_directory(state);
      chmod(state.c_str(), 0700);
      LinuxBackend backend;
      Engine e(state, backend, catalog);
      auto accepted = call(
          e, "start", {{"topologyHash", validated->hash}, {"idempotencyKey", "protocol-start"}});
      auto job = wait(e, accepted);
      auto run = call(e, "run", {{"id", accepted["runId"]}});
      write(dir / "start.json", job);
      auto a = container(run, "a"), b = container(run, "b");
      auto statusA = exec(a, {"/usr/local/bin/lab-node", "control", "status"}),
           statusB = exec(b, {"/usr/local/bin/lab-node", "control", "status"});
      Json item = {{"controller", argv[3]}, {"a", av},           {"b", bv}, {"run", run},
                   {"statusA", statusA},    {"statusB", statusB}};
      if (bv == "unsupported") {
        check(job["state"] == "failed" && job["error"] == "unsupported_gate_major",
              "unsupported wire major rejected before release");
        check(statusA["state"] == "held" && statusB["state"] == "held" &&
                  !statusB.value("releaseAttempted", true),
              "no node released before all protocol checks pass");
      } else {
        check(job["state"] == "succeeded", "mixed-version run starts: " + job.dump());
        check(statusA.value("protocolMinor", 0) == (av == "1.0" ? 0 : 1) &&
                  statusB.value("protocolMinor", 0) == (bv == "1.0" ? 0 : 1),
              "actual expected protocol minors observed");
        check(statusA["application"] == "app-a" && statusB["application"] == "app-b",
              "independent applications identified");
        auto ab = exec(a, {"/usr/local/bin/lab-node", "control", "probe", "10.231.17.2"}),
             ba = exec(b, {"/usr/local/bin/lab-node", "control", "probe", "10.231.17.1"});
        check(ab["probe"] == "received" && ba["probe"] == "received",
              "both-direction data interoperability");
        item["aToB"] = ab;
        item["bToA"] = ba;
        auto stopped = wait(e, call(e, "operate",
                                    {{"runId", run["id"]},
                                     {"expectedRevision", run["revision"]},
                                     {"operation", "stop"},
                                     {"idempotencyKey", "protocol-stop"}}));
        check(stopped["state"] == "succeeded", "compatible quiesce controls");
        check(exec(a, {"/usr/local/bin/lab-node", "control", "status"})["state"] == "held" &&
                  exec(b, {"/usr/local/bin/lab-node", "control", "status"})["state"] == "held",
              "both versions held after stop");
      }
      run = call(e, "run", {{"id", run["id"]}});
      auto clean = wait(e, call(e, "operate",
                                {{"runId", run["id"]},
                                 {"expectedRevision", run["revision"]},
                                 {"operation", "recover"},
                                 {"idempotencyKey", "protocol-cleanup"}}));
      check(clean["state"] == "succeeded", "protocol fixture cleanup");
      item["cleanup"] = clean;
      proof.push_back(item);
      write(root / "proof.json", proof);
    }
    std::cout << "T19 " << argv[3] << " controller passed " << proof.size() << " combinations\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    return 1;
  }
}
