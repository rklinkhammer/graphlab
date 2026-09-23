// Real Linux packet/capture benchmark. Reuse the existing owned-fixture helpers.
#define main m5_fixture_main
#include "../telemetry/linux.cpp"
#undef main
#include <algorithm>
#include <cstring>
#include <numeric>
#include <sys/resource.h>
using Clock = std::chrono::steady_clock;
Json host() {
  std::ifstream in("/proc/stat");
  std::string key;
  in >> key;
  std::uint64_t v[8]{};
  for (auto &n : v)
    in >> n;
  auto total = std::accumulate(std::begin(v), std::end(v), std::uint64_t{});
  Json out = {{"ticks", total}, {"busyTicks", total - v[3] - v[4]}};
  for (auto path : {"/proc/meminfo", "/proc/self/status", "/proc/vmstat"}) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      std::istringstream s(line);
      std::uint64_t n;
      s >> key >> n;
      if (key == "MemAvailable:" || key == "VmRSS:" || key == "pgpgin" || key == "pgpgout")
        out[key] = n;
    }
  }
  return out;
}
std::uint64_t bytes(const std::filesystem::path &dir) {
  std::uint64_t n = 0;
  for (const auto &p : std::filesystem::recursive_directory_iterator(dir))
    if (p.is_regular_file() &&
        (p.path().extension() == ".pcapng" || p.path().extension() == ".partial"))
      n += p.file_size();
  return n;
}
Json traffic(int pid, int rate, int seconds, const std::filesystem::path &output,
             const std::function<void()> &monitor = {}) {
  auto child = fork();
  if (child < 0)
    throw std::runtime_error("traffic fork");
  if (child == 0) {
    ns(pid);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "data0", 6))
      _exit(93);
    auto local = address("10.231.17.1");
    local.sin_port = 0;
    auto peer = address("10.231.17.2");
    peer.sin_port = htons(49000);
    if (bind(fd, (sockaddr *)&local, sizeof(local)) || connect(fd, (sockaddr *)&peer, sizeof(peer)))
      _exit(94);
    const auto started = Clock::now(), end = started + std::chrono::seconds(seconds);
    auto next = started;
    const auto spacing = std::chrono::nanoseconds(1000000000 / rate);
    std::vector<Clock::time_point> sent;
    std::vector<bool> seen;
    std::vector<double> latency;
    std::uint64_t invalid = 0, send_errors = 0;
    char packet[256]{};
    while (Clock::now() < end + std::chrono::milliseconds(500)) {
      auto now = Clock::now();
      if (now < end && now >= next) {
        std::uint64_t seq = sent.size();
        memcpy(packet, &seq, sizeof(seq));
        if (::send(fd, packet, sizeof(packet), 0) == sizeof(packet)) {
          sent.push_back(now);
          seen.push_back(false);
        } else
          ++send_errors;
        next += spacing;
      }
      ssize_t count;
      while ((count = recv(fd, packet, sizeof(packet), 0)) > 0) {
        std::uint64_t seq;
        memcpy(&seq, packet, sizeof(seq));
        if (count != sizeof(packet) || seq >= seen.size() || seen[seq]) {
          ++invalid;
          continue;
        }
        seen[seq] = true;
        latency.push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - sent[seq]).count());
      }
      if (Clock::now() < next) {
        pollfd p{fd, POLLIN, 0};
        poll(&p, 1, 1);
      }
    }
    std::sort(latency.begin(), latency.end());
    auto quantile = [&](double q) -> Json {
      return latency.empty()
                 ? Json(nullptr)
                 : Json(latency[std::min(latency.size() - 1, std::size_t(q * latency.size()))]);
    };
    Json result = {
        {"targetRequestsPerSecond", rate},
        {"durationSeconds", seconds},
        {"payloadBytes", 256},
        {"sent", sent.size()},
        {"received", latency.size()},
        {"sendErrors", send_errors},
        {"invalidOrDuplicate", invalid},
        {"lossFraction", sent.empty() ? 1.0 : double(sent.size() - latency.size()) / sent.size()},
        {"receivedRequestsPerSecond", double(latency.size()) / seconds},
        {"payloadBitsPerSecond", double(latency.size()) * 256 * 8 / seconds},
        {"rttP50Us", quantile(.5)},
        {"rttP95Us", quantile(.95)},
        {"rttP99Us", quantile(.99)}};
    std::ofstream out(output);
    out << result.dump(2);
    out.close();
    close(fd);
    _exit(0);
  }
  int status;
  auto deadline = Clock::now() + std::chrono::seconds(seconds + 15);
  auto next = Clock::now();
  try {
    for (;;) {
      auto result = waitpid(child, &status, WNOHANG);
      if (result == child)
        break;
      if (result < 0 || Clock::now() > deadline)
        throw std::runtime_error("traffic deadline");
      if (monitor && Clock::now() >= next) {
        monitor();
        next = Clock::now() + std::chrono::seconds(1);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } catch (...) {
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    throw;
  }
  if (status != 0)
    throw std::runtime_error("traffic child failed");
  std::ifstream result(output);
  return Json::parse(result);
}
int main(int argc, char **argv) {
  if (argc != 5 || geteuid()) {
    std::cerr << "Usage (root): m6_capacity SOURCE IMAGE_ID OUTPUT_DIR SECONDS_PER_SAMPLE\n";
    return 2;
  }
  int lockfd = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB))
    return 2;
  try {
    int seconds = std::stoi(argv[4]);
    const bool sustained = std::getenv("GRAPHLAB_CAPACITY_SUSTAINED") != nullptr;
    if (sustained && seconds != 300)
      throw std::runtime_error("sustained qualification requires 300-second samples");
    if (seconds < 5 || seconds > 300)
      throw std::runtime_error("sample seconds must be 5..300");
    const auto root = std::filesystem::absolute(argv[3]);
    if (root.string().size() > 40)
      throw std::runtime_error("use a short root-owned output parent for worker Unix sockets");
    if (std::filesystem::exists(root))
      throw std::runtime_error("output must be new");
    std::filesystem::create_directories(root);
    chmod(root.c_str(), 0700);
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    const bool triangle = std::getenv("GRAPHLAB_CAPACITY_TRIANGLE") != nullptr;
    const int budget_mib = std::getenv("GRAPHLAB_CAPACITY_BUDGET_MIB")
                               ? std::stoi(std::getenv("GRAPHLAB_CAPACITY_BUDGET_MIB"))
                           : sustained ? 6144
                                       : 256;
    if (sustained && budget_mib != 6144)
      throw std::runtime_error("sustained qualification requires declared 6 GiB budget");
    if (budget_mib < 256 || budget_mib > 6144)
      throw std::runtime_error("capture budget must be 256..6144 MiB");
    const std::uint64_t budget = std::uint64_t(budget_mib) * 1024 * 1024;
    Json report = {{"apiVersion", "graphlab.capacity/v1"},
                   {"observedAt", console::timestamp()},
                   {"captureBudgetBytes", budget},
                   {"shape", triangle ? "two Docker nodes on a redundant three-switch triangle"
                                      : "direct Docker a:data0 to b:data0"},
                   {"nodes", triangle ? 5 : 2},
                   {"edges", triangle ? 5 : 1},
                   {"measurementScope",
                    "host CPU and paging I/O include unrelated VM activity; RSS is "
                    "controller sample, not aggregate container/capture RSS"},
                   {"qualification", "bounded samples, not an extrapolated saturation limit"},
                   {"samples", Json::array()}};
    report["mode"] = sustained ? "sustained-m6" : "bounded-sweep";
    report["criteria"] = {{"maxLossFraction", .001},
                          {"minAchievedFraction", .99},
                          {"maxRttP95Us", 20000},
                          {"maxHostCpuPercent", 25},
                          {"minAvailableMemoryKiB", 2097152},
                          {"maxControllerRssGrowthKiB", 65536},
                          {"maxCaptureSourceDropIncrement", 0}};
    if (sustained)
      report["qualification"] = "declared ten-minute per-profile envelope; no extrapolation";
    bool qualified = true;
    for (auto profile : {"baseline", "telemetry", "capture"}) {
      auto folder = root / profile;
      std::filesystem::create_directory(folder);
      check(runtime::process(
                {(bin / "m2_tests").string(), "--fixtures", argv[1], folder.string(), argv[2]})
                    .code == 0,
            "capacity fixture");
      auto t = console::load(folder / "m2.yaml"),
           artifacts = console::load(folder / "artifacts.lock.json");
      if (triangle) {
        // Equal priorities let random bridge MACs change the forwarding path
        // between profiles, confounding the monitoring-overhead comparison.
        t["nodes"]["s1"]["policy"]["priority"] = 4096;
        t["nodes"]["s2"]["policy"]["priority"] = 8192;
        t["nodes"]["s3"]["policy"]["priority"] = 12288;
        report["rstpRoot"] = "s1";
      }
      if (!triangle) {
        for (auto id : {"s1", "s2", "s3"})
          t["nodes"].erase(id);
        t["nodes"]["a"]["ports"].erase("data1");
        t["nodes"]["b"]["ports"].erase("data1");
        t["edges"] =
            Json::array({{{"id", "wire"}, {"endpoints", Json::array({"a:data0", "b:data0"})}}});
      }
      const bool capture = std::string(profile) == "capture";
      t["capture"]["required"] = capture;
      std::ofstream(folder / "m2.yaml") << t.dump();
      console::Catalog catalog(folder, folder / "artifacts.lock.json");
      auto valid = lab_support::validate(t, artifacts);
      if (!valid)
        throw std::runtime_error("capacity topology invalid");
      auto state = folder / "state";
      std::filesystem::create_directory(state);
      chmod(state.c_str(), 0700);
      runtime::LinuxBackend backend;
      runtime::Engine engine(state, backend, catalog);
      Json run;
      auto cleanup = [&]() {
        if (run.is_null())
          return;
        run = call(engine, "run", {{"id", run["id"]}});
        auto j = wait(engine, call(engine, "operate",
                                   {{"runId", run["id"]},
                                    {"expectedRevision", run["revision"]},
                                    {"operation", "recover"},
                                    {"idempotencyKey", "capacity-cleanup"}}));
        if (j["state"] != "succeeded")
          throw std::runtime_error("capacity cleanup: " + j.dump());
      };
      try {
        auto accepted =
            call(engine, "start",
                 {{"topologyHash", valid->hash},
                  {"idempotencyKey", "capacity-start"},
                  {"developmentMode", !capture},
                  {"observationProfile", std::string(profile) == "baseline" ? "minimal" : "full"},
                  {"capturePolicy",
                   {{"runBytes", budget},
                    {"reserveBytes", 1024 * 1024},
                    {"rotateBytes", 16 * 1024 * 1024},
                    {"rotateSeconds", 10}}}});
        run = call(engine, "run", {{"id", accepted["runId"]}});
        auto j = wait(engine, accepted);
        if (j["state"] != "succeeded")
          throw std::runtime_error(j.dump());
        run = call(engine, "run", {{"id", run["id"]}});
        int apid = 0;
        for (const auto &r : run["resources"])
          if (r["kind"] == "container" && r["logical"] == "a")
            apid = runtime::docker_json("GET", "/v1.52/containers/" +
                                                   r["identity"]["id"].get<std::string>() +
                                                   "/json")["State"]["Pid"];
        if (apid <= 0)
          throw std::runtime_error("missing sender");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto profileRss = host()["VmRSS:"].get<std::uint64_t>();
        for (int repetition = 0; repetition < 2; ++repetition)
          for (int rate : sustained ? std::vector<int>{1000} : std::vector<int>{250, 1000, 4000}) {
            auto before = host();
            auto size = bytes(state);
            auto began = Clock::now();
            Json observations = Json::array();
            Json initial = capture ? backend.capture_control(run, "status") : Json::array();
            auto observe = [&] {
              auto current = call(engine, "run", {{"id", run["id"]}});
              Json compact = Json::array();
              for (const auto &c : current.value("captureObservations", Json::array()))
                compact.push_back({{"id", c.value("id", "")},
                                   {"invocationId", c.value("invocationId", "")},
                                   {"state", c.value("state", "unknown")},
                                   {"statistics", c.value("statistics", Json(nullptr))}});
              observations.push_back(
                  {{"elapsedSeconds", std::chrono::duration<double>(Clock::now() - began).count()},
                   {"host", host()},
                   {"state", current["state"]},
                   {"captureCoverage", current["captureCoverage"]},
                   {"workers", compact}});
              if (observations.size() % 30 == 0) {
                std::ofstream(folder / ("monitor-" + std::to_string(repetition) + ".json"))
                    << observations.dump(2);
                std::cout << "PROGRESS " << profile << " repeat=" << repetition
                          << " seconds=" << observations.back()["elapsedSeconds"] << std::endl;
              }
            };
            auto sample =
                traffic(apid, rate, seconds, folder / "traffic.json",
                        sustained ? std::function<void()>(observe) : std::function<void()>{});
            if (sustained)
              observe();
            auto after = host();
            const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
            auto delta = [&](const char *key) {
              return after[key].get<double>() - before[key].get<double>();
            };
            sample["profile"] = profile;
            sample["repeat"] = repetition;
            sample["runId"] = run["id"];
            sample["hostCpuBusyPercent"] = 100 * delta("busyTicks") / std::max(1.0, delta("ticks"));
            sample["controllerRssKiB"] = after["VmRSS:"];
            sample["hostMemAvailableKiB"] = after["MemAvailable:"];
            sample["hostPagingReadKiBPerSecond"] = delta("pgpgin") / elapsed;
            sample["hostPagingWriteKiBPerSecond"] = delta("pgpgout") / elapsed;
            sample["captureBytesPerSecond"] = double(bytes(state) - size) / elapsed;
            auto telemetry = call(engine, "telemetry", {{"runId", run["id"]}});
            sample["lastCollectionDurationNs"] = telemetry["lastCollectionDurationNs"];
            sample["captureObservation"] =
                capture ? backend.capture_control(run, "status") : Json(nullptr);
            auto current = call(engine, "run", {{"id", run["id"]}});
            sample["runState"] = current["state"];
            sample["withinSampleEnvelope"] =
                current["state"] == "ready" && sample["lossFraction"].get<double>() <= .01 &&
                sample["receivedRequestsPerSecond"].get<double>() >= rate * .95;
            if (sustained) {
              bool health = !observations.empty(), drops = true;
              std::uint64_t minMemory = before["MemAvailable:"], maxRss = before["VmRSS:"];
              auto validWorkers = [&](const Json &workers) {
                if (!capture)
                  return true;
                if (!workers.is_array() || workers.size() != initial.size() ||
                    initial.size() != t["edges"].size())
                  return false;
                for (const auto &c : workers) {
                  auto old = std::find_if(initial.begin(), initial.end(),
                                          [&](const Json &v) { return v["id"] == c["id"]; });
                  if (old == initial.end() || c["state"] != "active" ||
                      c["invocationId"] != (*old)["invocationId"] || !c["statistics"].is_object() ||
                      !c["statistics"].contains("dropped") ||
                      !c["statistics"]["dropped"].is_number() ||
                      !(*old)["statistics"]["dropped"].is_number() ||
                      c["statistics"]["dropped"] != (*old)["statistics"]["dropped"])
                    return false;
                }
                return true;
              };
              for (const auto &o : observations) {
                health = health && o["state"] == "ready" &&
                         (!capture || o["captureCoverage"] == "recording");
                drops = drops && validWorkers(o["workers"]);
                minMemory = std::min(minMemory, o["host"]["MemAvailable:"].get<std::uint64_t>());
                maxRss = std::max(maxRss, o["host"]["VmRSS:"].get<std::uint64_t>());
              }
              drops = drops && (!capture || validWorkers(sample["captureObservation"]));
              auto growth = maxRss > profileRss ? maxRss - profileRss : 0;
              sample["captureBefore"] = initial;
              auto monitorName =
                  std::string(profile) + "/monitor-" + std::to_string(repetition) + ".json";
              std::ofstream(root / monitorName) << observations.dump(2);
              sample["monitorFile"] = monitorName;
              sample["monitorCount"] = observations.size();
              sample["checks"] = {
                  {"readyThroughout", health},
                  {"workerContinuityAndZeroSourceDrops", drops},
                  {"deliveryLoss", sample["lossFraction"].get<double>() <= .001},
                  {"achievedRate", sample["receivedRequestsPerSecond"].get<double>() >= rate * .99},
                  {"rtt",
                   sample["rttP95Us"].is_number() && sample["rttP95Us"].get<double>() <= 20000},
                  {"hostCpu", sample["hostCpuBusyPercent"].get<double>() <= 25},
                  {"availableMemory", minMemory >= 2097152},
                  {"controllerRssGrowth", growth <= 65536}};
              sample["hostMemAvailableMinKiB"] = minMemory;
              sample["controllerRssMaxKiB"] = maxRss;
              sample["controllerRssGrowthFromProfileStartKiB"] = growth;
              bool passed = true;
              for (const auto &v : sample["checks"])
                passed = passed && v.get<bool>();
              sample["withinSampleEnvelope"] = passed;
              qualified = qualified && passed;
            }
            report["samples"].push_back(sample);
            if (sustained)
              report["withinDeclaredEnvelope"] = qualified;
            std::ofstream(root / "capacity.json") << report.dump(2);
            std::cout << "MEASURE " << profile << " " << rate << " "
                      << sample["withinSampleEnvelope"] << std::endl;
          }
        cleanup();
      } catch (...) {
        try {
          cleanup();
        } catch (const std::exception &e) {
          std::cerr << e.what() << '\n';
        }
        throw;
      }
    }
    std::cout << "Capacity report: " << root / "capacity.json" << '\n';
    return qualified ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
