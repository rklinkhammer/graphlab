// Explicit dedicated-host qualification; requires GRAPHLAB_TEST_CHECKPOINTS=ON.
#define main existing_terminal_main
#include "../terminal/linux.cpp"
#undef main
#include <atomic>
#include <set>
#include <sys/wait.h>

struct RecoveryBackend : runtime::Backend {
  runtime::LinuxBackend real;
  std::atomic<bool> pause{false}, entered{false};
  void configure(const std::filesystem::path &p) override { real.configure(p); }
  void preflight(const Json &t, const Json &a) override { real.preflight(t, a); }
  Json prepare(const Json &r, const Json &n) override { return real.prepare(r, n); }
  void remove(const Json &r, const Json &n) override {
    entered = true;
    while (pause)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    real.remove(r, n);
  }
  void activate(const Json &r) override { real.activate(r); }
  void gate(const Json &r, const std::string &a) override { real.gate(r, a); }
  Json observe(const Json &r) override { return real.observe(r); }
  Json capture_plan(const Json &r) override { return real.capture_plan(r); }
  Json capture_control(const Json &r, const std::string &a) override {
    return real.capture_control(r, a);
  }
  Json telemetry(const Json &r) override { return real.telemetry(r); }
  Json fault(const Json &r, const Json &f, const std::string &a) override {
    return real.fault(r, f, a);
  }
};
void audit(const Json &run, const std::filesystem::path &state,
           const std::filesystem::path &output) {
  Json proof;
  auto id = run["id"].get<std::string>();
  proof["containers"] = runtime::docker_json("GET", "/v1.52/containers/json?all=true");
  for (const auto &c : proof["containers"])
    check(c["Labels"].value("graphlab.run", "") != id, "no owned container or exec namespace");
  proof["networks"] = runtime::docker_json("GET", "/v1.52/networks");
  for (const auto &n : proof["networks"])
    check(n["Labels"].value("graphlab.run", "") != id, "no owned management network");
  auto links = runtime::process({"/usr/sbin/ip", "-j", "-d", "link", "show"});
  check(links.code == 0, "link inventory succeeds");
  proof["links"] = Json::parse(links.output);
  for (const auto &l : proof["links"])
    check(!l.value("ifalias", "").starts_with(id), "no owned veth TAP IFB or attached qdisc");
  for (const auto &r : run["resources"]) {
    auto name = runtime::resource_name(run, r["key"]);
    for (const auto &l : proof["links"])
      check(l["ifname"] != name, "no unlabelled resource link");
  }
  for (auto table : {"Bridge", "Port", "Interface"}) {
    auto r = runtime::process({"/usr/bin/ovs-vsctl", "--format=json", "list", table});
    check(r.code == 0 && r.output.find(id) == std::string::npos,
          "no owned orphan OVS " + std::string(table));
    proof[table] = Json::parse(r.output);
  }
  proof["recordings"] = Json::array();
  for (const auto &entry : std::filesystem::recursive_directory_iterator(state)) {
    check(!entry.is_socket(), "no owned runtime socket: " + entry.path().string());
    if (entry.path().filename() != "config.json")
      continue;
    auto d = console::load(entry.path());
    if (!d.contains("unit"))
      continue;
    auto u =
        runtime::process({"/usr/bin/systemctl", "list-units", "--all", "--no-legend", d["unit"]});
    check(u.code == 0 && u.output.empty(), "worker unit collected including child PTYs/QEMU");
    if (d.contains("nics"))
      for (const auto &nic : d["nics"])
        for (const auto &l : proof["links"])
          check(l["ifname"] != nic["tap"], "no partly created TAP");
    proof["recordings"].push_back(
        {{"directory", entry.path().parent_path().string()}, {"retained", true}});
  }
  auto netns = runtime::process({"/usr/sbin/ip", "netns", "list"});
  check(netns.code == 0, "namespace inventory");
  proof["namedNamespaces"] = netns.output;
  std::set<std::string> namespaces;
  for (const auto &r : run["resources"])
    if (r.contains("identity") && r["identity"].contains("endpoints"))
      for (const auto &ep : r["identity"]["endpoints"])
        if (ep.contains("namespaceInode"))
          namespaces.insert("net:[" + ep["namespaceInode"].get<std::string>() + "]");
  std::size_t inspected = 0;
  for (const auto &proc : std::filesystem::directory_iterator("/proc")) {
    auto pid = proc.path().filename().string();
    if (pid.find_first_not_of("0123456789") != std::string::npos)
      continue;
    std::error_code ec;
    auto net = std::filesystem::read_symlink(proc.path() / "ns/net", ec);
    if (!ec && namespaces.contains(net.string()))
      throw std::runtime_error("process remains in retired namespace: " + pid);
    if (ec && ec != std::errc::no_such_file_or_directory)
      throw std::runtime_error("process namespace inspection: " + ec.message());
    ec.clear();
    auto fds = std::filesystem::directory_iterator(proc.path() / "fd", ec);
    if (ec) {
      if (ec == std::errc::no_such_file_or_directory)
        continue;
      throw std::runtime_error("namespace FD inventory: " + ec.message());
    }
    for (const auto &fd : fds) {
      auto link = std::filesystem::read_symlink(fd.path(), ec);
      if (ec) {
        if (ec == std::errc::no_such_file_or_directory)
          continue;
        throw std::runtime_error("namespace FD readback: " + ec.message());
      }
      ++inspected;
      if (namespaces.contains(link.string()))
        throw std::runtime_error("owned namespace still held: " + fd.path().string());
    }
  }
  proof["namespaceFdInspection"] = {
      {"fdsInspected", inspected}, {"retiredNamespaces", namespaces}, {"remainingHandles", 0}};
  check(true, "no process retains an owned namespace FD");
  std::ofstream(output) << proof.dump(2) << '\n';
}
int main(int argc, char **argv) {
  if (argc == 3 && (std::string(argv[1]) == "--recover" || std::string(argv[1]) == "--audit") &&
      geteuid() == 0) {
    try {
      int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
      if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        return 2;
      for (const auto &entry : std::filesystem::directory_iterator(argv[2])) {
        auto dir = entry.path();
        if (!entry.is_directory() || !std::filesystem::exists(dir / "state/state.sqlite"))
          continue;
        console::Catalog catalog(dir, dir / "artifacts.lock.json");
        runtime::LinuxBackend backend;
        runtime::Engine e(dir / "state", backend, catalog);
        auto listing = call(e, "runs", Json::object());
        for (auto run : listing["items"]) {
          if (std::string(argv[1]) == "--audit") {
            run = call(e, "run", {{"id", run["id"]}});
            check(run["state"] == "destroyed", "audited fixture is destroyed");
            audit(run, dir / "state", dir / "fd-audit.json");
            continue;
          }
          auto j = wait(e, call(e, "operate",
                                {{"runId", run["id"]},
                                 {"expectedRevision", run["revision"]},
                                 {"operation", "recover"},
                                 {"idempotencyKey", "fixture-recover-" + console::random_hex(8)}}));
          check(j["state"] == "succeeded", "recover interrupted fixture: " + j.dump());
        }
      }
      return 0;
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
      return 1;
    }
  }
  if (argc != 5 || geteuid())
    return 2; // SOURCE IMAGE_ID PPC_INPUT ARM_INPUT
  try {
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    // Generate the same immutable guest inputs as the M4 fixture, without launching.
    auto guests = runtime::process(
        {(bin / "m4_linux").string(), argv[1], argv[3], argv[4], "--fixtures"}, 120);
    check(guests.code == 0, "guest fixture generation: " + guests.output);
    std::vector<std::filesystem::path> guest_roots;
    std::istringstream lines(guests.output);
    std::string line;
    while (std::getline(lines, line))
      if (line.starts_with("Evidence: \""))
        guest_roots.emplace_back(line.substr(11, line.size() - 12));
    check(guest_roots.size() == 2, "both TCG and KVM fixtures available");
    int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
      return 2;
    char tmp[] = "/tmp/gl6-crash-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    std::cout << "Evidence: " << root << std::endl;
    std::string sentinel = "m6s" + console::random_hex(4);
    check(runtime::process({"/usr/bin/ovs-vsctl", "add-br", sentinel}).code == 0,
          "create unrelated bridge sentinel");
    check(runtime::process({"/usr/sbin/ip", "link", "add", sentinel + "d", "type", "dummy"}).code ==
              0,
          "create unrelated link sentinel");
    auto container =
        runtime::docker_json("POST", "/v1.52/containers/create?name=" + sentinel,
                             {{"Image", argv[2]},
                              {"Entrypoint", Json::array({"/bin/sh", "-c", "sleep 3600"})},
                              {"HostConfig", {{"NetworkMode", "none"}}}})["Id"]
            .get<std::string>();
    runtime::docker_json("POST", "/v1.52/containers/" + container + "/start");
    std::vector<std::string> cases;
    for (auto profile : {"ppc", "arm"})
      for (auto point :
           {"intent", "config", "tap-created", "tap-owned", "tap-created.mgmt0", "tap-owned.mgmt0",
            "management", "overlay", "created", "readback", "completion"})
        cases.push_back(std::string(profile) + ":qemu." + point);
    for (auto point :
         {"intent", "worker", "exec-created", "exec-intent", "created", "readback", "completion"})
      cases.push_back(std::string("docker:terminal.") + point);
    for (auto profile : {"ppc", "arm"})
      for (auto point : {"intent", "worker", "readback", "completion"})
        cases.push_back(std::string(profile) + ":terminal." + point);
    for (auto op : {"apply", "remove"})
      for (auto point : {"intent", "created", "readback", "completion"})
        cases.push_back(std::string("docker:fault.") + op + "." + point);
    Json results = Json::array();
    for (const auto &name : cases) {
      std::cout << "CASE " << name << std::endl;
      auto colon = name.find(':');
      auto point = name.substr(colon + 1);
      bool vm = !name.starts_with("docker:");
      auto dir = root / ("case-" + std::to_string(results.size()));
      std::filesystem::create_directory(dir);
      auto state = dir / "state";
      std::filesystem::create_directory(state);
      chmod(state.c_str(), 0700);
      std::string topology = "m2.yaml";
      if (vm) {
        auto input = guest_roots[name.starts_with("ppc") ? 0 : 1];
        topology = "topology.yaml";
        for (auto file : {"topology.yaml", "artifacts.lock.json"})
          std::filesystem::copy_file(input / file, dir / file);
        for (auto sub : {"artifacts", "credentials"}) {
          std::filesystem::create_directory(state / sub);
          chmod((state / sub).c_str(), 0700);
          for (const auto &f : std::filesystem::directory_iterator(input / "state" / sub))
            std::filesystem::create_hard_link(f.path(), state / sub / f.path().filename());
        }
      } else
        check(runtime::process(
                  {(bin / "m2_tests").string(), "--fixtures", argv[1], dir.string(), argv[2]})
                      .code == 0,
              "Docker fixture");
      console::Catalog catalog(dir, dir / "artifacts.lock.json");
      auto hash = lab_support::validate(console::load(dir / topology),
                                        console::load(dir / "artifacts.lock.json"))
                      ->hash;
      auto pid = fork();
      if (pid == 0) {
        setenv("GRAPHLAB_CRASH_AT", point.c_str(), 1);
        try {
          runtime::LinuxBackend backend;
          runtime::Engine e(state, backend, catalog);
          auto start = call(e, "start",
                            {{"topologyHash", hash},
                             {"developmentMode", true},
                             {"idempotencyKey", "crash-start"}});
          auto job = wait(e, start);
          check(job["state"] == "succeeded", "start before fault: " + job.dump());
          auto run = call(e, "run", {{"id", start["runId"]}});
          if (vm) {
            Json descriptor;
            for (const auto &r : run["resources"])
              if (r["kind"] == "qemu")
                descriptor = r["identity"];
            bool ready = false;
            for (int i = 0; i < 1200 && !ready; ++i) {
              ready = terminal::request(descriptor, "status", run["controllerGeneration"])
                          .value("guestReady", false);
              if (!ready)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            check(ready, "guest ready before SSH session crash");
          }
          if (point.starts_with("terminal."))
            call(e, "terminal",
                 {{"runId", run["id"]},
                  {"node", vm ? "guest" : "a"},
                  {"operation", "open"},
                  {"params", Json::object()}});
          if (point.starts_with("fault.")) {
            auto f = call(e, "fault.apply",
                          {{"runId", run["id"]},
                           {"expectedRevision", run["revision"]},
                           {"idempotencyKey", "crash-fault"},
                           {"fault",
                            {{"edge", "a-s"},
                             {"kind", "netem"},
                             {"direction", "a-to-b"},
                             {"delayMs", 10},
                             {"durationSeconds", 300}}}});
            auto j = wait(e, f);
            check(j["state"] == "succeeded", "apply before remove");
            run = call(e, "run", {{"id", run["id"]}});
            wait(e, call(e, "fault.remove",
                         {{"runId", run["id"]},
                          {"expectedRevision", run["revision"]},
                          {"faultId", f["faultId"]},
                          {"idempotencyKey", "crash-remove"}}));
          }
        } catch (const std::exception &e) {
          std::cerr << e.what() << std::endl;
        }
        _exit(20);
      }
      int status = 0;
      waitpid(pid, &status, 0);
      check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "selected crash boundary reached: " + point);
      RecoveryBackend backend;
      runtime::Engine e(state, backend, catalog);
      auto runs = call(e, "runs", Json::object());
      check(runs["items"].size() == 1, "one durable uncertain run");
      auto run = call(e, "run", {{"id", runs["items"][0]["id"]}});
      std::ofstream(dir / "before-recovery.json") << run.dump(2);
      for (int repeat = 0; repeat < 2; repeat++) {
        run = call(e, "run", {{"id", run["id"]}});
        backend.pause = repeat == 0;
        backend.entered = false;
        auto op = call(e, "operate",
                       {{"runId", run["id"]},
                        {"expectedRevision", run["revision"]},
                        {"operation", "recover"},
                        {"idempotencyKey", "crash-recover-" + std::to_string(repeat)}});
        if (repeat == 0) {
          for (int i = 0; i < 1500 && !backend.entered; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          bool entered = backend.entered;
          bool denied = false;
          try {
            call(e, "cancel", {{"id", op["jobId"]}});
          } catch (const std::exception &ex) {
            denied = std::string(ex.what()) == "cleanup_operations_not_cancellable";
          }
          backend.pause = false;
          check(entered && denied, "cancellation during teardown denied; cleanup continues");
        }
        auto j = wait(e, op);
        std::ofstream(dir / ("recovery-" + std::to_string(repeat) + ".json")) << j.dump(2);
        check(j["state"] == "succeeded", "repeat scoped recovery: " + j.dump());
      }
      run = call(e, "run", {{"id", run["id"]}});
      audit(run, state, dir / "audit.json");
      check(runtime::process({"/usr/bin/ovs-vsctl", "br-exists", sentinel}).code == 0,
            "unrelated bridge survives");
      check(runtime::process({"/usr/sbin/ip", "link", "show", sentinel + "d"}).code == 0,
            "unrelated link survives");
      check(runtime::docker_json("GET", "/v1.52/containers/" + container +
                                            "/json")["State"]["Running"] == true,
            "unrelated container survives");
      results.push_back(
          {{"case", name}, {"status", "passed"}, {"audit", (dir / "audit.json").string()}});
      std::ofstream(root / "results.json") << results.dump(2);
    }
    runtime::docker_json("DELETE", "/v1.52/containers/" + container + "?force=true");
    check(runtime::process({"/usr/bin/ovs-vsctl", "del-br", sentinel}).code == 0,
          "remove test sentinel");
    check(runtime::process({"/usr/sbin/ip", "link", "delete", sentinel + "d"}).code == 0,
          "remove test sentinel link");
    std::cout << "T10/T17 crash matrix passed " << results.size() << " cases\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    return 1;
  }
}
