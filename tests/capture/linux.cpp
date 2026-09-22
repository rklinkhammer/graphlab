#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <iostream>
#include <linux/if_packet.h>
#include <net/if.h>
#include <pcap/pcap.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab::runtime;
namespace {
void check(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
void write(const std::filesystem::path &p, const Json &j) { std::ofstream(p) << j.dump(2); }
Json call(Engine &e, std::string m, Json p = Json::object()) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(Engine &e, const Json &a) {
  for (int i = 0; i < 900; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "running" && j["state"] != "queued")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("job timeout");
}
Json node(const Json &run, std::string action) {
  for (const auto &r : run["resources"])
    if (r["key"] == "node/a") {
      auto e = docker_json(
          "POST", "/v1.52/containers/" + r["identity"]["id"].get<std::string>() + "/exec",
          {{"AttachStdout", true},
           {"AttachStderr", true},
           {"Tty", true},
           {"Cmd", action == "probe"
                       ? Json::array({"/usr/local/bin/lab-node", "control", "probe", "10.231.17.2"})
                       : Json::array({"/usr/local/bin/lab-node", "control", action})}});
      auto result = docker_request("POST", "/v1.52/exec/" + e["Id"].get<std::string>() + "/start",
                                   {{"Detach", false}, {"Tty", true}});
      return Json::parse(result.body);
    }
  throw std::runtime_error("missing node");
}
Json op(Engine &e, const Json &run, std::string action, std::string key) {
  auto current = call(e, "run", {{"id", run["id"]}});
  return wait(e, call(e, "operate",
                      {{"runId", run["id"]},
                       {"expectedRevision", current["revision"]},
                       {"operation", action},
                       {"idempotencyKey", key}}));
}
struct Broken : Backend {
  bool quota = false;
  bool deny_write = false;
  bool stop_race = false;
  LinuxBackend real;
  void configure(const std::filesystem::path &p) override { real.configure(p); }
  void preflight(const Json &t, const Json &a) override { real.preflight(t, a); }
  Json prepare(const Json &r, const Json &s) override { return real.prepare(r, s); }
  void remove(const Json &r, const Json &s) override { real.remove(r, s); }
  void activate(const Json &r) override { real.activate(r); }
  void gate(const Json &r, const std::string &a) override { real.gate(r, a); }
  Json observe(const Json &r) override { return real.observe(r); }
  Json capture_plan(const Json &r) override {
    auto c = real.capture_plan(r);
    if (stop_race)
      return c;
    if (deny_write)
      c[0]["fileLimitBytes"] = 1;
    else if (quota) {
      c[0]["byteBudget"] = 262144;
      c[0]["rotateBytes"] = 4096;
    } else
      c[0]["filter"] = "(";
    return c;
  }
  Json capture_control(const Json &r, const std::string &a) override {
    if (stop_race && a == "stop" && !r["captures"].empty()) {
      auto unit = r["captures"][0]["unit"].get<std::string>();
      check(process({"/usr/bin/systemctl", "kill", "--signal=KILL", unit}).code == 0,
            "kill owned capture immediately before finalization");
      bool inactive = false;
      for (int i = 0; i < 100; ++i) {
        auto status =
            process({"/usr/bin/systemctl", "show", unit, "--property=ActiveState", "--value"});
        if (status.code == 0 && (status.output == "failed\n" || status.output == "inactive\n")) {
          inactive = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      check(inactive, "killed capture is inactive before finalization");
      stop_race = false;
    }
    return real.capture_control(r, a);
  }
};
} // namespace
int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--flood" && geteuid() == 0) {
    int fd = socket(AF_PACKET, SOCK_RAW, htons(0x88b5));
    sockaddr_ll address{};
    address.sll_family = AF_PACKET;
    address.sll_ifindex = if_nametoindex(argv[2]);
    address.sll_halen = 6;
    std::fill_n(address.sll_addr, 6, 255);
    if (fd < 0 || !address.sll_ifindex)
      return 3;
    unsigned char packet[1000]{};
    std::fill_n(packet, 6, 255);
    packet[6] = 2;
    packet[12] = 0x88;
    packet[13] = 0xb5;
    for (int i = 0; i < 512; ++i) {
      if (sendto(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) < 0)
        return 3;
      usleep(1000);
    }
    close(fd);
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--external-probe" && geteuid() == 0) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(49000);
    if (fd < 0 || inet_pton(AF_INET, argv[2], &address.sin_addr) != 1)
      return 3;
    constexpr char payload[] = "external-lease-check";
    sendto(fd, payload, sizeof(payload), 0, reinterpret_cast<sockaddr *>(&address),
           sizeof(address));
    pollfd event{fd, POLLIN, 0};
    int result = poll(&event, 1, 500);
    close(fd);
    return result > 0 ? 0 : 2;
  }
  if (argc != 5 || geteuid() != 0)
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  char name[] = "/tmp/gl3-XXXXXX";
  auto root = std::filesystem::path(mkdtemp(name));
  std::cout << "Evidence: " << root << std::endl;
  try {
    check(process({(std::filesystem::absolute(argv[0]).parent_path() / "m3_transport").string()})
                  .code == 0,
          "root worker disconnects cannot terminate controller with SIGPIPE");
    check(process({argv[2], "--fixtures", argv[1], root.string(), argv[3]}).code == 0,
          "fixture generated");
    auto artifacts = graphlab::console::load(root / "artifacts.lock.json");
    artifacts["workloads"]["app-b"]["image"] = "graphlab.local/app-b@" + std::string(argv[4]);
    write(root / "artifacts.lock.json", artifacts);
    auto topology = graphlab::console::load(root / "m2.yaml");
    topology["capture"]["required"] = true;
    topology["artifactLock"] = lab_support::digest(artifacts);
    write(root / "m2.yaml", topology);
    graphlab::console::Catalog catalog(root, root / "artifacts.lock.json");
    auto hash = lab_support::validate(topology, artifacts)->hash;
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    LinuxBackend backend;
    auto admission = Json{{"topologyHash", hash},
                          {"idempotencyKey", "m3-live-start-1"},
                          {"capturePolicy",
                           {{"runBytes", 32 * 1024 * 1024},
                            {"reserveBytes", 1024 * 1024},
                            {"rotateBytes", 1024 * 1024},
                            {"rotateSeconds", 2}}}};
    Json run;
    {
      Engine engine(state, backend, catalog);
      auto a = call(engine, "start", admission);
      auto job = wait(engine, a);
      check(job["state"] == "succeeded", "capture-first start: " + job.dump());
      run = call(engine, "run", {{"id", a["runId"]}});
      write(root / "ready.json", run);
      check(run["captures"].size() == topology["edges"].size(), "all data edges captured");
      for (const auto &s : run["captureObservations"])
        check(s["state"] == "active" && s["activatedAt"] <= run["releasedAt"],
              "worker activation precedes release");
      check(node(run, "probe").value("probe", "") == "received",
            "first released application probe delivered");
      for (const auto &c : run["captures"])
        graphlab::capture::worker_call(c, "rotate", run["controllerGeneration"]);
      bool found = false;
      int files = 0;
      for (const auto &c : run["captures"]) {
        auto m = graphlab::console::load(std::filesystem::path(c["directory"].get<std::string>()) /
                                         "manifest.json");
        for (const auto &s : m["segments"]) {
          auto path = std::filesystem::path(c["directory"].get<std::string>()) /
                      s["file"].get<std::string>();
          char error[PCAP_ERRBUF_SIZE]{};
          auto p = pcap_open_offline(path.c_str(), error);
          check(p != nullptr, "independent reader accepts edge PCAPNG");
          ++files;
          pcap_pkthdr *h;
          const unsigned char *b;
          int result;
          while ((result = pcap_next_ex(p, &h, &b)) == 1) {
            std::string bytes(reinterpret_cast<const char *>(b), h->caplen);
            found = found || bytes.find("graphlab-probe-") != std::string::npos;
          }
          check(result == -2, "independent reader reaches clean EOF");
          pcap_close(p);
        }
      }
      check(found && files >= int(topology["edges"].size()),
            "earliest application payload recorded with valid files for every edge");
      auto list = call(engine, "artifacts", {{"runId", run["id"]}});
      check(!list["items"].empty(), "artifact listing");
      Json item;
      for (const auto &candidate : list["items"])
        if (candidate.contains("id") && candidate["state"] == "closed") {
          item = candidate;
          break;
        }
      check(!item.is_null(), "closed artifact available alongside partial stream");
      auto chunk =
          call(engine, "artifact", {{"runId", run["id"]}, {"id", item["id"]}, {"offset", "0"}});
      check(chunk["sha256"] == item["sha256"] && !chunk["base64"].get<std::string>().empty(),
            "scoped closed artifact download");
      check(op(engine, run, "stop", "m3-live-stop")["state"] == "succeeded",
            "quiesce finalizes captures");
      auto original_manifest =
          std::filesystem::path(run["captures"][0]["directory"].get<std::string>()) /
          "manifest.json";
      auto manifest_hash = graphlab::capture::file_hash(original_manifest);
      auto replay =
          process({(std::filesystem::absolute(argv[0]).parent_path() / "lab-capture").string(),
                   "--config", (original_manifest.parent_path() / "config.json").string()});
      check(replay.code != 0 &&
                replay.output.find("capture_identity_already_used") != std::string::npos &&
                graphlab::capture::file_hash(original_manifest) == manifest_hash,
            "worker identity cannot be restarted or overwrite prior evidence");
      check(op(engine, run, "resume", "m3-live-resume")["state"] == "succeeded", "resume rearms");
      run = call(engine, "run", {{"id", run["id"]}});
      check(run["captureEpoch"] == "2", "new epoch on resume");
      auto victim = run["captures"][0];
      check(process({"/usr/bin/systemctl", "kill", "--signal=KILL", victim["unit"]}).code == 0,
            "owned worker killed");
      auto failure = std::chrono::steady_clock::now();
      for (int i = 0; i < 130; ++i) {
        run = call(engine, "run", {{"id", run["id"]}});
        if (run["state"] == "reconciling")
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      check(run["captureCoverage"] == "incomplete" && node(run, "status")["state"] == "held",
            "worker loss degrades coverage and quiesces");
      write(root / "failure.json",
            {{"run", run},
             {"observedMilliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - failure)
                                          .count()}});
      check(op(engine, run, "recover", "m3-live-recover")["state"] == "succeeded",
            "failed worker recovery cleans resources");
      admission["idempotencyKey"] = "m3-live-start-2";
      auto next = call(engine, "start", admission);
      check(wait(engine, next)["state"] == "succeeded", "second capture run");
      run = call(engine, "run", {{"id", next["runId"]}});
    }
    auto before =
        graphlab::capture::worker_call(run["captures"][0], "status", run["controllerGeneration"]);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    auto after =
        graphlab::capture::worker_call(run["captures"][0], "status", run["controllerGeneration"]);
    check(before["invocationId"] == after["invocationId"] &&
              before["sourceSeq"] != after["sourceSeq"],
          "independent worker records through agent outage");
    {
      Engine restarted(state, backend, catalog);
      auto current = call(restarted, "run", {{"id", run["id"]}});
      check(current.contains("adoptedAt") && node(run, "status")["state"] == "held",
            "restart adopts workers and holds traffic");
      bool denied = false;
      try {
        graphlab::capture::worker_call(run["captures"][0], "status", run["controllerGeneration"]);
      } catch (const Failure &) {
        denied = true;
      }
      check(denied, "stale controller fenced");
      check(op(restarted, current, "recover", "m3-adopt-cleanup")["state"] == "succeeded",
            "adopted worker cleanup");
      admission["idempotencyKey"] = "m3-live-start-3";
      auto a = call(restarted, "start", admission);
      check(wait(restarted, a)["state"] == "succeeded", "lease-outage run");
      run = call(restarted, "run", {{"id", a["runId"]}});
    }
    auto lost = std::chrono::steady_clock::now();
    check(
        process({"/usr/bin/systemctl", "kill", "--signal=KILL", run["captures"][0]["unit"]}).code ==
            0,
        "capture lost during agent outage");
    Json status;
    for (int i = 0; i < 125; ++i) {
      status = node(run, "status");
      if (status["state"] == "held")
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - lost)
                       .count();
    check(status["state"] == "held" && elapsed <= 12000,
          "node lease bounds outage quiescence to 12 seconds");
    for (const auto &r : run["resources"])
      if (r["key"] == "node/b") {
        auto c = docker_json("GET", "/v1.52/containers/" + r["identity"]["id"].get<std::string>() +
                                        "/json");
        auto probe = process(
            {"/usr/bin/nsenter",
             "--net=/proc/" + std::to_string(c["State"]["Pid"].get<int>()) + "/ns/net", "--",
             std::filesystem::absolute(argv[0]).string(), "--external-probe", "10.231.17.1"});
        check(probe.code == 2, "external UDP receives no application echo after lease expiry");
      }
    write(root / "lease.json", {{"milliseconds", elapsed}, {"status", status}});
    {
      Engine recovered(state, backend, catalog);
      check(op(recovered, run, "recover", "m3-outage-recover")["state"] == "succeeded",
            "outage recovery cleanup");
    }
    auto racestate = root / "stop-race";
    std::filesystem::create_directory(racestate);
    chmod(racestate.c_str(), 0700);
    Broken race;
    race.stop_race = true;
    {
      Engine engine(racestate, race, catalog);
      admission["idempotencyKey"] = "m3-stop-race";
      auto a = call(engine, "start", admission);
      check(wait(engine, a)["state"] == "succeeded", "stop-race run armed");
      auto r = call(engine, "run", {{"id", a["runId"]}});
      check(op(engine, r, "stop", "m3-race-stop")["state"] == "succeeded",
            "stop accounts for killed worker");
      r = call(engine, "run", {{"id", a["runId"]}});
      check(r["captureCoverage"] == "closed-incomplete" &&
                r["captureObservations"][0]["state"] == "interrupted",
            "real stop race persists coverage gap and interrupted observation");
      check(node(r, "status")["state"] == "held", "stop-race workload remains held");
      check(op(engine, r, "destroy", "m3-race-cleanup")["state"] == "succeeded",
            "stop-race cleanup");
      check(op(engine, r, "destroy", "m3-race-repeat")["state"] == "succeeded" &&
                call(engine, "run", {{"id", r["id"]}})["captureCoverage"] == "closed-incomplete",
            "repeated real cleanup preserves incomplete coverage");
    }
    auto brokenstate = root / "broken";
    std::filesystem::create_directory(brokenstate);
    chmod(brokenstate.c_str(), 0700);
    Broken broken;
    {
      Engine engine(brokenstate, broken, catalog);
      admission["idempotencyKey"] = "m3-broken-filter";
      auto a = call(engine, "start", admission);
      check(wait(engine, a)["state"] == "failed", "invalid capture filter fails start");
      auto r = call(engine, "run", {{"id", a["runId"]}});
      check(node(r, "status")["state"] == "held", "failed activation never releases live workload");
      check(op(engine, r, "recover", "m3-broken-recover")["state"] == "succeeded",
            "failed-arm resources cleaned");
    }
    auto quotastate = root / "quota";
    std::filesystem::create_directory(quotastate);
    chmod(quotastate.c_str(), 0700);
    Broken quota;
    quota.quota = true;
    {
      Engine engine(quotastate, quota, catalog);
      admission["idempotencyKey"] = "m3-live-quota";
      auto a = call(engine, "start", admission);
      check(wait(engine, a)["state"] == "succeeded", "quota test run armed");
      auto r = call(engine, "run", {{"id", a["runId"]}});
      check(process({std::filesystem::absolute(argv[0]).string(), "--flood",
                     r["captures"][0]["interface"]})
                    .code == 0,
            "bounded owned-interface traffic fixture");
      for (int i = 0; i < 100; ++i) {
        r = call(engine, "run", {{"id", a["runId"]}});
        if (r["state"] == "reconciling")
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      auto m = graphlab::console::load(
          std::filesystem::path(r["captures"][0]["directory"].get<std::string>()) /
          "manifest.json");
      check(m.value("error", "") == "capture_quota_exhausted" &&
                r["captureCoverage"] == "incomplete" && node(r, "status")["state"] == "held",
            "real quota exhaustion reports loss and quiesces");
      check(m["segments"].size() > 1, "rotation under packet load preserves multiple segments");
      write(root / "quota-result.json", m);
      check(op(engine, r, "recover", "m3-quota-cleanup")["state"] == "succeeded",
            "quota failure cleanup");
    }
    auto deniedstate = root / "denied";
    std::filesystem::create_directory(deniedstate);
    chmod(deniedstate.c_str(), 0700);
    Broken denied;
    denied.deny_write = true;
    {
      Engine engine(deniedstate, denied, catalog);
      admission["idempotencyKey"] = "m3-deny-write";
      auto a = call(engine, "start", admission);
      check(wait(engine, a)["state"] == "failed",
            "OS write limit prevents capture activation acknowledgement");
      auto r = call(engine, "run", {{"id", a["runId"]}});
      check(node(r, "status")["state"] == "held", "write-denied arm holds traffic");
      check(op(engine, r, "recover", "m3-denied-cleanup")["state"] == "succeeded",
            "write-denied cleanup");
    }
    for (bool isolated : {false, true}) {
      auto folder = root / (isolated ? "isolated" : "direct");
      std::filesystem::create_directory(folder);
      chmod(folder.c_str(), 0700);
      auto t = topology;
      if (isolated) {
        t = graphlab::console::load(std::filesystem::path(argv[1]) / "topologies/isolated.yaml");
        t["artifactLock"] = lab_support::digest(artifacts);
      } else {
        for (auto id : {"s1", "s2", "s3"})
          t["nodes"].erase(id);
        t["edges"] =
            Json::array({{{"id", "direct"}, {"endpoints", Json::array({"a:data0", "b:data0"})}}});
      }
      write(folder / "m2.yaml", t);
      write(folder / "artifacts.lock.json", artifacts);
      graphlab::console::Catalog selected(folder, folder / "artifacts.lock.json");
      auto valid = lab_support::validate(t, artifacts);
      check(bool(valid), "alternate graph validates");
      auto private_state = folder / "state";
      std::filesystem::create_directory(private_state);
      chmod(private_state.c_str(), 0700);
      LinuxBackend implementation;
      Engine engine(private_state, implementation, selected);
      admission["topologyHash"] = valid->hash;
      admission["idempotencyKey"] = isolated ? "m3-isolated" : "m3-direct";
      auto a = call(engine, "start", admission);
      auto job = wait(engine, a);
      check(job["state"] == "succeeded",
            std::string(isolated ? "isolated" : "direct Docker-Docker") +
                " capture start: " + job.dump());
      auto r = call(engine, "run", {{"id", a["runId"]}});
      write(folder / "ready.json", r);
      check(r["captures"].size() == t["edges"].size(), "no fictitious or omitted edge captures");
      if (!isolated) {
        check(r["captures"][0]["namespacePath"] != "/proc/self/ns/net",
              "direct edge capture uses verified container namespace");
        check(node(r, "probe").value("probe", "") == "received",
              "direct Docker-Docker application traffic");
      }
      check(op(engine, r, "destroy", "m3-alternate-cleanup")["state"] == "succeeded",
            "alternate graph cleanup");
    }
    std::cout << "M3 Linux tests passed" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << "; evidence " << root << std::endl;
    return 1;
  }
  close(lock);
  return 0;
}
