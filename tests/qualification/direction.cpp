// Numbered one-way probes observed independently at both endpoint namespaces.
#define main m5_fixture_main
#include "../telemetry/linux.cpp"
#undef main
#include <cstring>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <set>
#include <sys/ioctl.h>
void offloads(int target, bool enabled) {
  auto pid = fork();
  if (pid == 0) {
    ns(target);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    for (auto command : {ETHTOOL_SGRO, ETHTOOL_SGSO}) {
      ethtool_value value{static_cast<unsigned>(command), unsigned(enabled)};
      ifreq request{};
      std::strcpy(request.ifr_name, "data0");
      request.ifr_data = reinterpret_cast<char *>(&value);
      if (ioctl(fd, SIOCETHTOOL, &request))
        _exit(95);
      value.cmd = command == ETHTOOL_SGRO ? ETHTOOL_GGRO : ETHTOOL_GGSO;
      if (ioctl(fd, SIOCETHTOOL, &request) || value.data != unsigned(enabled))
        _exit(96);
    }
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  check(status == 0,
        enabled ? "GRO/GSO enabled with kernel readback" : "GRO/GSO disabled with kernel readback");
}
Json numbered(int from_pid, int to_pid, const char *from, const char *to,
              const std::filesystem::path &path) {
  int ready[2];
  if (pipe(ready))
    throw std::runtime_error("probe pipe");
  auto receiver = fork();
  if (receiver == 0) {
    close(ready[0]);
    ns(to_pid);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    auto local = address(to);
    if (bind(fd, (sockaddr *)&local, sizeof(local)))
      _exit(97);
    char r = 'r';
    write(ready[1], &r, 1);
    close(ready[1]);
    std::set<std::uint64_t> seen;
    std::vector<double> delays;
    unsigned duplicates = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd event{fd, POLLIN, 0};
      poll(&event, 1, 10);
      std::uint64_t payload[32]{};
      if (recv(fd, payload, sizeof(payload), 0) != sizeof(payload))
        continue;
      if (payload[0] >= 40 || !seen.insert(payload[0]).second) {
        ++duplicates;
        continue;
      }
      delays.push_back(double(telemetry::monotonic() - payload[1]) / 1000000.0);
    }
    double sum = 0;
    for (auto d : delays)
      sum += d;
    std::ofstream(path) << Json{
        {"sent", 40},
        {"received", seen.size()},
        {"duplicates", duplicates},
        {"meanOneWayMs", delays.empty() ? Json(nullptr) : Json(sum / delays.size())},
        {"sequences", seen}}.dump();
    _exit(0);
  }
  close(ready[1]);
  pollfd event{ready[0], POLLIN, 0};
  if (poll(&event, 1, 3000) != 1)
    throw std::runtime_error("receiver readiness");
  char r;
  if (read(ready[0], &r, 1) != 1)
    throw std::runtime_error("receiver failed");
  close(ready[0]);
  auto sender = fork();
  if (sender == 0) {
    ns(from_pid);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    auto local = address(from);
    local.sin_port = 0;
    auto peer = address(to);
    if (bind(fd, (sockaddr *)&local, sizeof(local)))
      _exit(98);
    for (unsigned i = 0; i < 40; ++i) {
      std::uint64_t payload[32]{};
      payload[0] = i;
      payload[1] = telemetry::monotonic();
      if (sendto(fd, payload, sizeof(payload), 0, (sockaddr *)&peer, sizeof(peer)) !=
          sizeof(payload))
        _exit(99);
      usleep(5000);
    }
    _exit(0);
  }
  int sent, received;
  waitpid(sender, &sent, 0);
  waitpid(receiver, &received, 0);
  check(sent == 0 && received == 0, "numbered sender and receiver completed");
  std::ifstream file(path);
  return Json::parse(file);
}
int main(int argc, char **argv) {
  if (argc != 3 || geteuid())
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    char pattern[] = "/tmp/gl6-dir-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(pattern));
    std::cout << "Evidence: " << root << std::endl;
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    check(runtime::process(
              {(bin / "m2_tests").string(), "--fixtures", argv[1], root.string(), argv[2]})
                  .code == 0,
          "direction fixture generated");
    auto t = console::load(root / "m2.yaml"),
         artifacts = console::load(root / "artifacts.lock.json");
    for (auto id : {"s1", "s2", "s3"})
      t["nodes"].erase(id);
    for (auto id : {"a", "b"})
      t["nodes"][id]["ports"].erase("data1");
    t["edges"] =
        Json::array({{{"id", "wire"}, {"endpoints", Json::array({"a:data0", "b:data0"})}}});
    t["capture"]["required"] = true;
    std::ofstream(root / "m2.yaml") << t.dump();
    console::Catalog catalog(root, root / "artifacts.lock.json");
    auto valid = lab_support::validate(t, artifacts);
    check(bool(valid), "direction topology validates");
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    runtime::LinuxBackend backend;
    runtime::Engine e(state, backend, catalog);
    auto admission =
        call(e, "start", {{"topologyHash", valid->hash}, {"idempotencyKey", "m6-direction-start"}});
    Json run = call(e, "run", {{"id", admission["runId"]}});
    auto cleanup = [&] {
      run = call(e, "run", {{"id", run["id"]}});
      check(wait(e, call(e, "operate",
                         {{"runId", run["id"]},
                          {"expectedRevision", run["revision"]},
                          {"operation", "recover"},
                          {"idempotencyKey", "m6-direction-cleanup"}}))["state"] == "succeeded",
            "direction cleanup");
    };
    try {
      check(wait(e, admission)["state"] == "succeeded", "captured direct edge ready");
      run = call(e, "run", {{"id", run["id"]}});
      int apid = 0, bpid = 0;
      for (const auto &r : run["resources"])
        if (r["kind"] == "container") {
          int pid = runtime::docker_json("GET", "/v1.52/containers/" +
                                                    r["identity"]["id"].get<std::string>() +
                                                    "/json")["State"]["Pid"];
          (r["logical"] == "a" ? apid : bpid) = pid;
        }
      Json report = Json::array();
      unsigned iteration = 0;
      for (bool enabled : {false, true}) {
        offloads(apid, enabled);
        offloads(bpid, enabled);
        for (bool loss : {false, true}) {
          run = call(e, "run", {{"id", run["id"]}});
          auto a = call(e, "fault.apply",
                        {{"runId", run["id"]},
                         {"expectedRevision", run["revision"]},
                         {"idempotencyKey", "m6-direction-" + std::to_string(iteration)},
                         {"fault",
                          {{"edge", "wire"},
                           {"kind", "netem"},
                           {"direction", "a-to-b"},
                           {"delayMs", loss ? 0 : 80},
                           {"lossPercent", loss ? 100 : 0},
                           {"durationSeconds", 30}}}});
          check(wait(e, a)["state"] == "succeeded", "asymmetric policy installed");
          auto forward = numbered(apid, bpid, "10.231.17.1", "10.231.17.2", root / "forward.json");
          auto reverse = numbered(bpid, apid, "10.231.17.2", "10.231.17.1", root / "reverse.json");
          check(forward["received"] == (loss ? 0 : 40) && reverse["received"] == 40,
                "endpoint sequences confirm asymmetric delivery");
          if (!loss)
            check(forward["meanOneWayMs"].get<double>() >= 70 &&
                      forward["meanOneWayMs"].get<double>() >
                          reverse["meanOneWayMs"].get<double>() + 50,
                  "80 ms delay measured only in forward direction");
          report.push_back({{"groGso", enabled},
                            {"lossPolicy", loss},
                            {"forward", forward},
                            {"reverse", reverse},
                            {"captureSourceCounters", backend.capture_control(run, "status")},
                            {"kernelTelemetry", runtime::LinuxBackend{}.telemetry(run)}});
          std::ofstream(root / "direction.json") << report.dump(2);
          run = call(e, "run", {{"id", run["id"]}});
          check(wait(e, call(e, "fault.remove",
                             {{"runId", run["id"]},
                              {"expectedRevision", run["revision"]},
                              {"faultId", a["faultId"]},
                              {"idempotencyKey",
                               "m6-remove-" + std::to_string(iteration++)}}))["state"] ==
                    "succeeded",
                "policy removed");
        }
      }
    } catch (...) {
      cleanup();
      throw;
    }
    cleanup();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
