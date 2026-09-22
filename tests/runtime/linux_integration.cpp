#include <fcntl.h>
#include <fstream>
#include <graphlab/runtime.hpp>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace graphlab::runtime;
namespace {
struct CrashBackend : Backend {
  LinuxBackend real;
  int boundary, current = 0;
  bool after;
  CrashBackend(int b, bool a) : boundary(b), after(a) {}
  bool enter() {
    bool crash = current++ == boundary;
    if (crash && !after)
      _exit(91);
    return crash;
  }
  void leave(bool crash) {
    if (crash && after)
      _exit(91);
  }
  void preflight(const Json &t, const Json &a) override { real.preflight(t, a); }
  Json prepare(const Json &run, const Json &r) override {
    bool crash = enter();
    auto result = real.prepare(run, r);
    leave(crash);
    return result;
  }
  void remove(const Json &run, const Json &r) override {
    auto c = enter();
    real.remove(run, r);
    leave(c);
  }
  void activate(const Json &r) override {
    auto c = enter();
    real.activate(r);
    leave(c);
  }
  void gate(const Json &r, const std::string &a) override {
    auto c = enter();
    real.gate(r, a);
    leave(c);
  }
  Json observe(const Json &r) override { return real.observe(r); }
};
void check(bool b, const std::string &why) {
  if (!b)
    throw std::runtime_error(why);
  std::cout << "PASS " << why << std::endl;
}
Json call(Engine &e, const std::string &m, Json p = Json::object()) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(Engine &e, const Json &a) {
  for (int i = 0; i < 900; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("job timeout");
}
Json exec(const std::string &id, const std::vector<std::string> &command) {
  auto e = docker_json(
      "POST", "/v1.52/containers/" + id + "/exec",
      {{"AttachStdout", true}, {"AttachStderr", true}, {"Tty", true}, {"Cmd", command}});
  auto r = docker_request("POST", "/v1.52/exec/" + e["Id"].get<std::string>() + "/start",
                          {{"Detach", false}, {"Tty", true}});
  if (r.status != 200)
    throw std::runtime_error("exec failed");
  return Json::parse(r.body);
}
std::string container(const Json &run, const std::string &node) {
  for (const auto &r : run["resources"])
    if (r["key"] == "node/" + node)
      return r["identity"]["id"];
  throw std::runtime_error("no container");
}
void write(const std::filesystem::path &p, const Json &j) { std::ofstream(p) << j.dump(2) << '\n'; }
} // namespace
int main(int argc, char **argv) {
  if (argc != 5 || geteuid() != 0) {
    std::cerr
        << "Usage (root Linux only): m2_linux SOURCE FIXTURE_GENERATOR IMAGE_A_ID IMAGE_B_ID\n";
    return 2;
  }
  int hostlock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (hostlock < 0 || flock(hostlock, LOCK_EX | LOCK_NB)) {
    std::cerr << "host executor busy\n";
    return 2;
  }
  char temporary[] = "/tmp/graphlab-m2-live-XXXXXX";
  auto p = mkdtemp(temporary);
  if (!p)
    return 2;
  std::filesystem::path work = p;
  std::cout << "Evidence directory: " << work << std::endl;
  try {
    auto generated = process({argv[2], "--fixtures", argv[1], work.string(), argv[3]});
    check(generated.code == 0, "fixture generated from real image identity");
    auto lock = graphlab::console::load(work / "artifacts.lock.json");
    lock["workloads"]["app-b"]["image"] = "graphlab.local/app-b@" + std::string(argv[4]);
    write(work / "artifacts.lock.json", lock);
    auto topology = graphlab::console::load(work / "m2.yaml");
    topology["artifactLock"] = lab_support::digest(lock);
    topology["management"]["networks"]["mgmt"] = {{"subnet", "172.31.246.0/24"},
                                                  {"dynamicPool", "172.31.246.128/25"},
                                                  {"gateway", "172.31.246.1"},
                                                  {"externalAccess", false}};
    topology["managementAttachments"] = Json::array(
        {{{"endpoint", "a:mgmt0"}, {"network", "mgmt"}, {"address", "172.31.246.10/24"}},
         {{"endpoint", "b:mgmt0"}, {"network", "mgmt"}, {"address", "172.31.246.11/24"}}});
    // Keep an unrelated sentinel to detect broad OVS/container cleanup.
    auto sentinel = "gs" + graphlab::console::random_hex(5);
    check(process({"/usr/bin/ovs-vsctl", "add-br", sentinel}).code == 0, "sentinel bridge created");
    struct Sentinel {
      std::string name;
      ~Sentinel() { process({"/usr/bin/ovs-vsctl", "--if-exists", "del-br", name}); }
    } sentinel_guard{sentinel};
    LinuxBackend backend;
    for (bool isolated : {false, true}) {
      auto folder = work / (isolated ? "vlan-negative" : "vlan-positive");
      std::filesystem::create_directory(folder);
      chmod(folder.c_str(), 0700);
      auto t = topology;
      t["nodes"]["s2"]["ports"]["workload"]["vlan"]["access"] = isolated ? 200 : 100;
      write(folder / "topology.yaml", t);
      write(folder / "artifacts.lock.json", lock);
      graphlab::console::Catalog catalog(folder, folder / "artifacts.lock.json");
      auto valid = lab_support::validate(t, lock);
      check(bool(valid), "runtime topology validated");
      Engine engine(folder, backend, catalog);
      auto accepted = call(engine, "start",
                           {{"topologyHash", valid->hash},
                            {"idempotencyKey", "integration-start"},
                            {"developmentMode", true}});
      auto job = wait(engine, accepted);
      auto run = call(engine, "run", {{"id", accepted["runId"]}});
      write(folder / "started.json", run);
      write(folder / "job.json", job);
      auto teardown = [&] {
        auto current = call(engine, "run", {{"id", accepted["runId"]}});
        return wait(engine, call(engine, "operate",
                                 {{"runId", current["id"]},
                                  {"expectedRevision", current["revision"]},
                                  {"operation", "destroy"},
                                  {"idempotencyKey", "integration-destroy"}}));
      };
      try {
        check(job["state"] == "succeeded", "real Docker/OVS run ready: " + job.dump());
        auto changed = run;
        Json changed_edge;
        for (auto &r : changed["resources"])
          if (r["kind"] == "edge")
            for (auto &endpoint : r["identity"]["endpoints"])
              if (endpoint.contains("namespaceInode")) {
                endpoint["namespaceInode"] = "0";
                changed_edge = r;
              }
        bool rejected = false;
        try {
          backend.activate(changed);
        } catch (const Failure &e) {
          rejected = std::string(e.what()) == "namespace_identity_changed";
        }
        check(rejected, "resume rejects mismatched namespace identity");
        rejected = false;
        try {
          backend.remove(changed, changed_edge);
        } catch (const Failure &e) {
          rejected = std::string(e.what()) == "namespace_identity_changed";
        }
        check(rejected, "cleanup rejects mismatched namespace identity");
        auto a = container(run, "a"), b = container(run, "b");
        check(exec(a, {"/usr/local/bin/lab-node", "control", "status"})["state"] == "released",
              "C++ workload released");
        auto result = exec(a, {"/usr/local/bin/lab-node", "control", "probe", "10.231.17.2"});
        write(folder / "probe.json", result);
        check(result.value("probe", "") == (isolated ? "timeout" : "received"),
              isolated ? "cross-VLAN traffic denied" : "same-VLAN UDP delivered");
        check(exec(a, {"/usr/local/bin/lab-node", "control", "probe", "172.31.246.11"})
                      .value("probe", "") == "timeout",
              "management address does not expose data echo");
        check(run["captureCoverage"] == "unavailable-development-mode",
              "no capture coverage advertised");
        auto stop = call(engine, "operate",
                         {{"runId", run["id"]},
                          {"expectedRevision", run["revision"]},
                          {"operation", "stop"},
                          {"idempotencyKey", "integration-stop"}});
        check(wait(engine, stop)["state"] == "succeeded", "quiesce completes");
        check(exec(a, {"/usr/local/bin/lab-node", "control", "probe", "10.231.17.2"})
                      .value("error", "") == "gate_held",
              "held gate denies application probe");
        auto resume = call(engine, "operate",
                           {{"runId", run["id"]},
                            {"expectedRevision", stop["revision"]},
                            {"operation", "resume"},
                            {"idempotencyKey", "integration-resume"}});
        check(wait(engine, resume)["state"] == "succeeded", "resume completes");
      } catch (...) {
        auto result = teardown();
        write(folder / "cleanup.json", result);
        throw;
      }
      check(teardown()["state"] == "succeeded", "owned resource teardown completes");
      check(process({"/usr/bin/ovs-vsctl", "br-exists", sentinel}).code == 0,
            "unrelated bridge preserved");
      for (const auto &r : run["resources"]) {
        if (r["kind"] == "container")
          check(docker_request("GET", "/v1.52/containers/" +
                                          r["identity"]["id"].get<std::string>() + "/json")
                        .status == 404,
                "container removed");
        if (r["kind"] == "bridge")
          check(process({"/usr/bin/ovs-vsctl", "br-exists", resource_name(run, r["key"])}).code !=
                    0,
                "bridge removed");
      }
    }
    const auto count = resources({{"id", "fixture"}, {"topology", topology}}).size();
    // prepare N, activate/release, stop, resume activation/release,
    // destroy quiesce, remove N: every Backend mutation boundary.
    for (std::size_t boundary = 0; boundary < count * 2 + 6; ++boundary)
      for (bool after : {false, true}) {
        auto folder = work / ("crash-" + std::to_string(boundary) + (after ? "-after" : "-before"));
        std::filesystem::create_directory(folder);
        chmod(folder.c_str(), 0700);
        write(folder / "topology.yaml", topology);
        write(folder / "artifacts.lock.json", lock);
        graphlab::console::Catalog catalog(folder, folder / "artifacts.lock.json");
        auto valid = lab_support::validate(topology, lock);
        auto child = fork();
        if (child == 0) {
          CrashBackend crashing(static_cast<int>(boundary), after);
          Engine engine(folder, crashing, catalog);
          auto accepted = call(engine, "start",
                               {{"topologyHash", valid->hash},
                                {"idempotencyKey", "crash-start-key"},
                                {"developmentMode", true}});
          if (wait(engine, accepted)["state"] != "succeeded")
            _exit(93);
          for (const auto &operation : {"stop", "resume", "destroy"}) {
            auto run = call(engine, "run", {{"id", accepted["runId"]}});
            auto next = call(engine, "operate",
                             {{"runId", run["id"]},
                              {"expectedRevision", run["revision"]},
                              {"operation", operation},
                              {"idempotencyKey", std::string("crash-") + operation + "-key"}});
            if (wait(engine, next)["state"] != "succeeded")
              _exit(94);
          }
          _exit(92);
        }
        if (child < 0)
          throw std::runtime_error("fork failed");
        int status;
        waitpid(child, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 91, "abrupt exit at mutation boundary " +
                                                                  std::to_string(boundary) +
                                                                  (after ? " after" : " before"));
        Engine recovered(folder, backend, catalog);
        auto entries = call(recovered, "runs")["items"];
        check(entries.size() == 1, "interrupted run retained");
        auto run = call(recovered, "run", {{"id", entries[0]["id"]}});
        check(run["state"] == "reconciling", "interrupted run fenced");
        auto cleaned = wait(recovered, call(recovered, "operate",
                                            {{"runId", run["id"]},
                                             {"expectedRevision", run["revision"]},
                                             {"operation", "recover"},
                                             {"idempotencyKey", "crash-recover-key"}}));
        write(folder / "recovery.json", cleaned);
        check(cleaned["state"] == "succeeded", "real recovery cleans uncertain resources");
        check(process({"/usr/bin/ovs-vsctl", "br-exists", sentinel}).code == 0,
              "sentinel survives crash recovery");
      }
    std::cout << "M2 Linux runtime tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "; retained evidence: " << work << '\n';
    return 1;
  }
  close(hostlock);
  return 0;
}
