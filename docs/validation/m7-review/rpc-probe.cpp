#define main m4_fixture_main
#include "tests/qemu/linux.cpp"
#undef main
#include <arpa/inet.h>
#include <pcap/pcap.h>
#include <sys/wait.h>
int probe() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "data0", 6);
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(49000);
  inet_pton(AF_INET, "10.233.17.2", &target.sin_addr);
  std::string payload = "M7-independent-probe";
  for (int i = 0; i < 3; ++i) {
    sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr *>(&target),
           sizeof(target));
    char bytes[512];
    auto n = recv(fd, bytes, sizeof(bytes), 0);
    if (n > 0 && std::string(bytes, n) == "reply:" + payload) {
      close(fd);
      return 0;
    }
  }
  close(fd);
  return 1;
}
void audit(const Json &run, const std::filesystem::path &state) {
  auto id = run["id"].get<std::string>();
  for (const auto &c : runtime::docker_json("GET", "/v1.52/containers/json?all=true"))
    check(c["Labels"].value("graphlab.run", "") != id, "no owned container remains");
  auto links = Json::parse(runtime::process({"/usr/sbin/ip", "-j", "link"}).output);
  for (const auto &l : links)
    check(!l.value("ifalias", "").starts_with(id), "no owned link remains");
  for (auto table : {"Bridge", "Port", "Interface", "QoS"}) {
    auto out = runtime::process({"/usr/bin/ovs-vsctl", "--format=json", "list", table});
    check(!out.code && out.output.find(id) == std::string::npos, "no owned OVS rows remain");
  }
  for (const auto &entry : std::filesystem::recursive_directory_iterator(state)) {
    check(!entry.is_socket(), "no stale worker socket: " + entry.path().string());
    if (entry.path().filename() == "config.json") {
      auto d = console::load(entry.path());
      if (d.contains("unit")) {
        auto out = runtime::process(
            {"/usr/bin/systemctl", "list-units", "--all", "--no-legend", d["unit"]});
        check(!out.code && out.output.empty(), "worker unit collected");
      }
    }
  }
}
Json operate(runtime::Engine &engine, Json run, std::string key) {
  run = call(engine, "run", {{"id", run["id"]}});
  return wait(engine, call(engine, "operate",
                           {{"runId", run["id"]},
                            {"expectedRevision", run["revision"]},
                            {"operation", "recover"},
                            {"idempotencyKey", key}}));
}
std::string output(const Json &d, const Json &run) {
  std::string out, sequence = "0";
  for (int i = 0; i < 10000; ++i) {
    auto page =
        terminal::request(d, "replay", run["controllerGeneration"], {{"sequence", sequence}});
    for (const auto &r : page["records"])
      if (r["type"] == 1)
        out += terminal::decode(r["base64"]);
    if (page["next"] == sequence)
      return out;
    sequence = page["next"];
  }
  throw std::runtime_error("replay limit");
}

void rpcFault(runtime::Engine &engine, Json &run, Json &fault, bool apply) {
  run = call(engine, "run", {{"id", run["id"]}});
  Json params = {{"runId", run["id"]}, {"expectedRevision", run["revision"]},
                 {"idempotencyKey", console::random_hex(16)}};
  if (apply) {
    params["fault"] = {{"edge", fault["edge"]}, {"kind", "netem"},
                       {"direction", fault["direction"]}, {"lossPercent", 100},
                       {"durationSeconds", fault.value("reviewDuration", 60)}};
    auto admitted = call(engine, "fault.apply", params);
    fault["id"] = admitted["faultId"];
    auto job = wait(engine, admitted);
    check(job["state"] == "succeeded", "RPC fault apply journal: " + job.dump());
    run = call(engine, "run", {{"id", run["id"]}});
    fault["placement"] = run["faults"][fault["id"].get<std::string>()]["placement"];
  } else {
    params["faultId"] = fault["id"];
    auto job = wait(engine, call(engine, "fault.remove", params));
    check(job["state"] == "succeeded", "RPC fault removal journal: " + job.dump());
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--probe")
    return probe();
  if (argc == 3 && std::string(argv[1]) == "--recover" && !geteuid()) {
    try {
      int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
      if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        return 2;
      auto dir = std::filesystem::path(argv[2]);
      console::Catalog catalog(dir, dir / "artifacts.lock.json");
      runtime::LinuxBackend backend;
      runtime::Engine engine(dir / "state", backend, catalog);
      auto runs = call(engine, "runs");
      for (const auto &run : runs["items"]) {
        check(operate(engine, run, "m7-manual-recover")["state"] == "succeeded",
              "retained fixture recovery");
        audit(run, dir / "state");
      }
      return 0;
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
      return 1;
    }
  }
  if ((argc != 6 && argc != 7) ||
      (argc == 7 && std::string(argv[6]) != "--crash" && std::string(argv[6]) != "--ownership"))
    return 2; // SOURCE PPC_INPUT ARM_INPUT APP_IMAGE RUNNER_IMAGE [--crash]
  if (geteuid())
    return 2;
  try {
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    auto generated = runtime::process(
        {(bin / "m4_linux").string(), argv[1], argv[2], argv[3], "--fixtures"}, 60);
    check(!generated.code, "guest fixtures generated");
    std::vector<std::filesystem::path> bases;
    std::istringstream lines(generated.output);
    std::string line;
    while (std::getline(lines, line))
      if (line.starts_with("Evidence: \""))
        bases.emplace_back(line.substr(11, line.size() - 12));
    check(bases.size() == 2, "two guest architecture inputs");
    int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
      return 2;
    char tmp[] = "/tmp/gl7-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    std::cout << "Evidence: " << root << std::endl;
    bool crashing = argc == 7 && std::string(argv[6]) == "--crash";
    bool ownership = argc == 7 && std::string(argv[6]) == "--ownership";
    struct Case {
      int arch;
      bool container, pair;
      std::string checkpoint;
    };
    std::vector<Case> cases;
    if (crashing) {
      for (auto cp :
           {"attachment.bridge", "attachment.veth-created", "attachment.veth-moved",
            "attachment.port.0", "attachment.port.1", "attachment.qos.0", "attachment.qos.1"})
        cases.push_back({1, false, false, cp});
      for (auto cp : {"qemu.runner-created", "qemu.runner-started"})
        cases.push_back({1, true, false, cp});
    } else if (ownership) {
      cases.push_back({1, false, false, ""});
      cases.push_back({1, false, true, ""});
    } else
      for (int arch : {0, 1})
        for (bool container : {false, true})
          for (bool pair : {false, true})
            cases.push_back({arch, container, pair, ""});
    auto sentinel = "m7" + console::random_hex(4);
    check(!runtime::process({"/usr/bin/ovs-vsctl", "add-br", sentinel, "--", "set", "Bridge",
                             sentinel, "external_ids:m7-sentinel=" + root.string()})
               .code,
          "unrelated sentinel bridge created");
    std::ofstream(root / "sentinel.txt") << sentinel;
    int ordinal = 0;
    for (const auto &test : cases) {
      auto dir = root / std::to_string(ordinal++);
      std::filesystem::copy(bases[test.arch], dir, std::filesystem::copy_options::recursive);
      auto state = dir / "state";
      auto t = console::load(dir / "topology.yaml"),
           artifacts = console::load(dir / "artifacts.lock.json");
      t["nodes"].erase("s1");
      t["nodes"]["guest"]["addresses"]["data0"] = "10.233.17.2/24";
      if (test.container)
        artifacts["workloads"]["guest-linux"]["vm"]["runnerImage"] = argv[5];
      if (test.pair) {
        t["nodes"]["peer"] = t["nodes"]["guest"];
      } else {
        auto app = console::load(std::filesystem::path(argv[1]) /
                                 "topologies/artifacts.lock.json")["workloads"]["app-a"];
        app["image"] = "graphlab.local/app-a@" + std::string(argv[4]);
        artifacts["workloads"]["app-a"] = app;
        t["nodes"]["peer"] = {
            {"kind", "docker"}, {"workload", "app-a"}, {"ports", t["nodes"]["guest"]["ports"]}};
      }
      t["nodes"]["peer"]["addresses"]["data0"] = "10.233.17.1/24";
      t["managementAttachments"].push_back(
          {{"endpoint", "peer:mgmt0"}, {"network", "control"}, {"address", "172.31.243.11/24"}});
      t["edges"] = Json::array(
          {{{"id", "direct"}, {"endpoints", Json::array({"guest:data0", "peer:data0"})}}});
      if (ownership && !test.pair)
        std::reverse(t["edges"][0]["endpoints"].begin(), t["edges"][0]["endpoints"].end());
      t["artifactLock"] = lab_support::digest(artifacts);
      write(dir / "artifacts.lock.json", artifacts);
      write(dir / "topology.yaml", t);
      auto valid = lab_support::validate(t, artifacts);
      check(bool(valid), "direct topology validates");
      auto unsupported = t;
      unsupported["backend"]["switchIsolation"] = "namespace-ovs";
      auto rejected = lab_support::validate(unsupported, artifacts);
      check(!rejected && rejected.error().code == "unsupported_backend",
            "unqualified switch isolation rejected before effects");
      console::Catalog catalog(dir, dir / "artifacts.lock.json");
      Json run;
      if (crashing) {
        auto child = fork();
        if (!child) {
          setenv("GRAPHLAB_CRASH_AT", test.checkpoint.c_str(), 1);
          runtime::LinuxBackend backend;
          runtime::Engine engine(state, backend, catalog);
          wait(engine,
               call(engine, "start",
                    {{"topologyHash", valid->hash}, {"idempotencyKey", "m7-backend-start"}}));
          _exit(3);
        }
        int status;
        waitpid(child, &status, 0);
        check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
              "executor killed at " + test.checkpoint);
      }
      {
        runtime::LinuxBackend backend;
        runtime::Engine engine(state, backend, catalog);
        if (!crashing) {
          auto admission =
              call(engine, "start",
                   {{"topologyHash", valid->hash}, {"idempotencyKey", "m7-backend-start"}});
          auto job = wait(engine, admission);
          write(dir / "start.json", job);
          check(job["state"] == "succeeded", "direct start arch=" + std::to_string(test.arch) +
                                                 " container=" + std::to_string(test.container) +
                                                 " pair=" + std::to_string(test.pair) + " " +
                                                 job.dump());
          run = call(engine, "run", {{"id", admission["runId"]}});
          check(run["captures"].size() == 1, "one modeled edge yields one required capture");
          check(run["captures"][0]["canonicalEndpoint"] == "guest:data0",
                "capture selects guest TAP in either endpoint order");
          Json edge;
          for (const auto &r : run["resources"])
            if (r["kind"] == "edge")
              edge = r;
          check(edge["identity"].contains("attachment"),
                "attachment separately inventoried from logical switches");
          auto bridge = edge["identity"]["attachment"]["name"].get<std::string>();
          auto addresses =
              runtime::process({"/usr/sbin/ip", "-j", "address", "show", "dev", bridge});
          check(Json::parse(addresses.output)[0]["addr_info"].empty(), "attachment has no host IP");
          for (const auto &r : run["resources"])
            if (r["kind"] == "qemu") {
              auto d = r["identity"];
              bool ready = false;
              for (int i = 0; i < 1200; ++i) {
                if (terminal::request(d, "status", run["controllerGeneration"])
                        .value("guestReady", false)) {
                  ready = true;
                  break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              check(ready, "actual guest network readiness");
            }
          if (!test.pair) {
            Json c;
            for (const auto &r : run["resources"])
              if (r["kind"] == "container")
                c = runtime::docker_json(
                    "GET", "/v1.52/containers/" + r["identity"]["id"].get<std::string>() + "/json");
            auto args = std::vector<std::string>{
                "/usr/bin/nsenter",
                "--net=/proc/" + std::to_string(c["State"]["Pid"].get<int>()) + "/ns/net", "--",
                std::filesystem::absolute(argv[0]).string(), "--probe"};
            check(!runtime::process(args).code,
                  "Docker to guest and guest reply delivered independently of capture");
            Json fault = {{"id", "m7-loss"},
                          {"edge", "direct"},
                          {"direction", ownership ? "a-to-b" : "b-to-a"},
                          {"delayMs", 0},
                          {"lossPercent", 100}};
            
            rpcFault(engine, run, fault, true);
            check(runtime::process(args).code != 0,
                  "100 percent Docker-to-guest loss at declared egress");
            rpcFault(engine, run, fault, false);
            check(!runtime::process(args).code, "delivery restored after fault removal");
            fault["direction"] = ownership ? "b-to-a" : "a-to-b";
            
            rpcFault(engine, run, fault, true);
            check(runtime::process(args).code != 0,
                  "100 percent guest-to-Docker loss at opposite host egress");
            rpcFault(engine, run, fault, false);
            check(!runtime::process(args).code, "reverse delivery restored after fault removal");
          } else {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            for (const auto &r : run["resources"])
              if (r["kind"] == "qemu") {
                auto d = r["identity"];
                auto token = terminal::request(d, "acquire", run["controllerGeneration"],
                                               {{"owner", "m7"}})["token"];
                terminal::request(
                    d, "input", run["controllerGeneration"],
                    {{"owner", "m7"}, {"token", token}, {"base64", terminal::encode("status\n")}});
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto transcript = output(d, run);
                std::ofstream(dir / (r["logical"].get<std::string>() + "-serial.txt"))
                    << transcript;
                auto pos = transcript.rfind("received=");
                check(pos != std::string::npos && std::stoul(transcript.substr(pos + 9)) > 0,
                      "each guest independently reports received data packets");
              }
          }
          if (ownership && test.pair) {
            std::vector<Json> guests;
            for (const auto &resource : run["resources"])
              if (resource["kind"] == "qemu")
                guests.push_back(resource["identity"]);
            auto received = [&](const Json &d) {
              auto before = output(d, run).size();
              auto token = terminal::request(d, "acquire", run["controllerGeneration"],
                                             {{"owner", "m7"}, {"takeover", true}})["token"];
              terminal::request(
                  d, "input", run["controllerGeneration"],
                  {{"owner", "m7"}, {"token", token}, {"base64", terminal::encode("status\n")}});
              for (int i = 0; i < 30; ++i) {
                auto transcript = output(d, run);
                auto pos = transcript.rfind("received=");
                if (pos != std::string::npos && pos >= before &&
                    transcript.find('\n', pos) != std::string::npos)
                  return std::stoul(transcript.substr(pos + 9));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              throw std::runtime_error("guest counter response missing");
            };
            Json measurements = Json::array();
            for (int cycle = 0; cycle < 4; ++cycle) {
              int direction = cycle % 2;
              Json fault = {{"id", "m7-pair-loss"},
                            {"edge", "direct"},
                            {"direction", direction ? "b-to-a" : "a-to-b"},
                            {"delayMs", 0},
                            {"lossPercent", 100}};
              
              fault["reviewDuration"] = cycle == 3 ? 25 : 60;
              rpcFault(engine, run, fault, true);
              std::this_thread::sleep_for(std::chrono::seconds(1));
              auto before = received(guests[1 - direction]), opposite = received(guests[direction]);
              std::this_thread::sleep_for(std::chrono::seconds(2));
              auto after = received(guests[1 - direction]),
                   opposite_after = received(guests[direction]);
              measurements.push_back({{"direction", direction},
                                      {"before", before},
                                      {"after", after},
                                      {"oppositeBefore", opposite},
                                      {"oppositeAfter", opposite_after},
                                      {"placement", fault["placement"]}});
              write(dir / "direction-measurements.json", measurements);
              check(after == before && opposite_after > opposite,
                    "guest-pair loss is directional at opposite TAP TX");
              if (cycle == 3) {
                bool expired = false;
                for (int attempt = 0; attempt < 300; ++attempt) {
                  auto current = call(engine, "run", {{"id", run["id"]}});
                  if (current["faults"][fault["id"].get<std::string>()]["state"] == "removed") {
                    expired = true;
                    break;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                check(expired, "RPC direct guest fault expires and journals removal");
              } else rpcFault(engine, run, fault, false);
              std::this_thread::sleep_for(std::chrono::seconds(1));
              check(received(guests[1 - direction]) > before,
                    "guest-pair directional delivery restored");
            }
          }
          runtime::LinuxBackend observer;
          observer.configure(state);
          auto telemetry = observer.telemetry(run);
          check(telemetry.size() == 1 && telemetry[0]["valid"] == true,
                "direct link telemetry mapped");
          write(dir / "telemetry.json", telemetry);
          write(dir / "run.json", run);
          if (test.container) {
            Json d;
            for (const auto &r : run["resources"])
              if (r["kind"] == "qemu") {
                d = r["identity"];
                break;
              }
            auto c = runtime::docker_json("GET", "/v1.52/containers/" +
                                                     d["runnerId"].get<std::string>() + "/json");
            check(c["HostConfig"]["Privileged"] == false &&
                      c["HostConfig"]["ReadonlyRootfs"] == true &&
                      c["HostConfig"]["NetworkMode"] == "host",
                  "runner has explicit shared-network boundary and constrained rootfs");
            check(
                !runtime::process({"/usr/bin/systemctl", "kill", "--signal=KILL", d["unit"]}).code,
                "terminal supervisor killed");
            bool stopped = false;
            for (int i = 0; i < 80; ++i) {
              c = runtime::docker_json("GET", "/v1.52/containers/" +
                                                  d["runnerId"].get<std::string>() + "/json");
              if (!c["State"]["Running"].get<bool>()) {
                stopped = true;
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            check(stopped, "independent container watchdog kills orphan QEMU within eight seconds");
          }
        } else
          run = call(engine, "runs")["items"][0];
        if (ownership && !test.pair) {
          Json edge;
          for (const auto &resource : run["resources"])
            if (resource["kind"] == "edge")
              edge = resource;
          auto bridge = edge["identity"]["attachment"]["name"].get<std::string>();
          auto foreign = "m7f" + console::random_hex(4);
          check(!runtime::process({"/usr/sbin/ip", "link", "add", foreign, "type", "dummy"}).code,
                "foreign port fixture created");
          check(!runtime::process({"/usr/bin/ovs-vsctl", "add-port", bridge, foreign}).code,
                "foreign port injected into owned attachment");
          auto failed = operate(engine, run, "foreign-port-refused");
          write(dir / "foreign-recovery.json", failed);
          check(failed["state"] == "failed",
                "foreign attachment port blocks deletion and reports residuals");
          check(!runtime::process({"/usr/bin/ovs-vsctl", "get", "Port", foreign, "_uuid"}).code,
                "foreign port survives cleanup refusal");
          check(!runtime::process({"/usr/bin/ovs-vsctl", "del-port", foreign}).code,
                "test removes only injected foreign port");
          check(!runtime::process({"/usr/sbin/ip", "link", "delete", foreign}).code,
                "test removes its foreign dummy");
        }
        auto recovered = operate(engine, run, "recover-one");
        write(dir / "recovery.json", recovered);
        check(recovered["state"] == "succeeded", "scoped recovery: " + recovered.dump());
        check(operate(engine, run, "recover-two")["state"] == "succeeded", "repeated recovery");
      }
      audit(run, state);
      auto observed_sentinel = runtime::process(
          {"/usr/bin/ovs-vsctl", "get", "Bridge", sentinel, "external_ids:m7-sentinel"});
      check(!observed_sentinel.code &&
                observed_sentinel.output.find(root.string()) != std::string::npos,
            "unrelated sentinel survives repeated recovery");
      bool first_packet = false;
      unsigned packets = 0;
      for (const auto &capture : run.value("captures", Json::array())) {
        for (const auto &entry :
             std::filesystem::directory_iterator(capture["directory"].get<std::string>()))
          if (entry.path().extension() == ".pcapng") {
            char error[PCAP_ERRBUF_SIZE]{};
            auto file = pcap_open_offline(entry.path().c_str(), error);
            check(file != nullptr, "independent reader accepts direct edge PCAPNG");
            pcap_pkthdr *header;
            const unsigned char *data;
            int result;
            while ((result = pcap_next_ex(file, &header, &data)) == 1) {
              ++packets;
              std::string bytes(reinterpret_cast<const char *>(data), header->caplen);
              first_packet = first_packet || bytes.find("graphlab-guest-0") != std::string::npos;
            }
            check(result == -2, "independent packet read reaches clean EOF");
            pcap_close(file);
          }
      }
      if (!crashing)
        check(packets > 0 && first_packet,
              "earliest numbered guest traffic retained at declared capture point");
    }
    check(!runtime::process({"/usr/bin/ovs-vsctl", "del-br", sentinel}).code,
          "test sentinel removed explicitly");
    std::cout << "M7 Linux backend tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
