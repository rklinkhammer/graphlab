// Real tagged-trunk and bounded RSTP failover qualification on owned fixtures.
#define main m5_fixture_main
#include "../telemetry/linux.cpp"
#undef main
#include <pcap/pcap.h>
Json node_probe(const std::string &id) {
  auto exec = runtime::docker_json(
      "POST", "/v1.52/containers/" + id + "/exec",
      {{"AttachStdout", true},
       {"AttachStderr", true},
       {"Tty", true},
       {"Cmd", Json::array({"/usr/local/bin/lab-node", "control", "probe", "10.231.17.2"})}});
  auto result =
      runtime::docker_request("POST", "/v1.52/exec/" + exec["Id"].get<std::string>() + "/start",
                              {{"Detach", false}, {"Tty", true}});
  return Json::parse(result.body);
}
int main(int argc, char **argv) {
  if (argc != 3 || geteuid())
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    char tmp[] = "/tmp/gl6-net-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    std::cout << "Evidence: " << root << std::endl;
    check(runtime::process(
              {(bin / "m2_tests").string(), "--fixtures", argv[1], root.string(), argv[2]})
                  .code == 0,
          "network fixture generated");
    auto topology = console::load(root / "m2.yaml"),
         artifacts = console::load(root / "artifacts.lock.json");
    // Deterministic root bridge; all inter-switch ports are tagged trunks.
    topology["nodes"]["s1"]["policy"]["priority"] = 4096;
    topology["nodes"]["s2"]["policy"]["priority"] = 8192;
    topology["nodes"]["s3"]["policy"]["priority"] = 12288;
    std::ofstream(root / "m2.yaml") << topology.dump();
    console::Catalog catalog(root, root / "artifacts.lock.json");
    auto valid = lab_support::validate(topology, artifacts);
    check(bool(valid), "trunk triangle validates");
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    runtime::LinuxBackend backend;
    runtime::Engine engine(state, backend, catalog);
    auto admission = call(engine, "start",
                          {{"topologyHash", valid->hash},
                           {"idempotencyKey", "m6-network"},
                           {"developmentMode", true}});
    Json run = call(engine, "run", {{"id", admission["runId"]}});
    auto cleanup = [&] {
      run = call(engine, "run", {{"id", run["id"]}});
      check(wait(engine, call(engine, "operate",
                              {{"runId", run["id"]},
                               {"expectedRevision", run["revision"]},
                               {"operation", "recover"},
                               {"idempotencyKey", "m6-network-cleanup"}}))["state"] == "succeeded",
            "network cleanup");
    };
    try {
      check(wait(engine, admission)["state"] == "succeeded", "network run ready");
      run = call(engine, "run", {{"id", run["id"]}});
      std::ofstream(root / "run.json") << run.dump(2);
      std::string container, link;
      for (const auto &r : run["resources"]) {
        if (r["kind"] == "container" && r["logical"] == "a")
          container = r["identity"]["id"];
        if (r["kind"] == "edge" && r["logical"] == "l12")
          link = r["identity"]["endpoints"][0]["name"];
      }
      check(!container.empty() && !link.empty(), "owned trunk identity resolved");
      auto roles = runtime::LinuxBackend{}.telemetry(run);
      std::ofstream(root / "tree-before.json") << roles.dump(2);
      check(roles.dump().find("Forwarding") != std::string::npos &&
                roles.dump().find("Discarding") != std::string::npos,
            "RSTP forwarding and alternate state observable");
      char error[PCAP_ERRBUF_SIZE]{};
      auto *capture = pcap_open_live(link.c_str(), 65535, 1, 20, error);
      if (!capture)
        throw std::runtime_error(error);
      struct Close {
        pcap_t *p;
        ~Close() { pcap_close(p); }
      } close{capture};
      check(pcap_setnonblock(capture, 1, error) == 0, "independent trunk reader armed");
      check(node_probe(container).value("probe", "") == "received",
            "same-VLAN positive control before cut");
      bool tagged = false;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline && !tagged) {
        pcap_pkthdr *header;
        const unsigned char *packet;
        auto result = pcap_next_ex(capture, &header, &packet);
        if (result == 1 && header->caplen >= 18)
          tagged = packet[12] == 0x81 && packet[13] == 0 &&
                   (((packet[14] & 15) << 8) | packet[15]) == 100;
        else if (result < 0)
          throw std::runtime_error("trunk capture read");
        if (tagged) {
          auto *dump = pcap_dump_open(capture, (root / "tagged-trunk.pcap").c_str());
          if (!dump)
            throw std::runtime_error("trunk evidence open");
          pcap_dump(reinterpret_cast<unsigned char *>(dump), header, packet);
          auto status = pcap_dump_flush(dump);
          pcap_dump_close(dump);
          check(status == 0, "tagged frame retained for independent readback");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      check(tagged, "independent packet reader observes VLAN 100 tag on trunk");
      // Predeclared selected-runtime target; fail rather than widening it after measurement.
      constexpr int target_ms = 15000;
      auto began = std::chrono::steady_clock::now();
      check(runtime::process({"/usr/sbin/ip", "link", "set", "dev", link, "down"}).code == 0,
            "cut owned forwarding trunk");
      bool delivered = false;
      double elapsed = 0;
      do {
        delivered = node_probe(container).value("probe", "") == "received";
        elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                .count();
      } while (!delivered && elapsed < target_ms);
      std::ofstream(root / "failover.json") << Json{{"targetMs", target_ms},
                                                    {"firstDeliveredProbeMs", elapsed},
                                                    {"delivered", delivered},
                                                    {"trunk", link},
                                                    {"vlan", 100}}
                                                   .dump(2);
      check(runtime::process({"/usr/sbin/ip", "link", "set", "dev", link, "up"}).code == 0,
            "restore owned trunk");
      check(delivered && elapsed <= target_ms,
            "RSTP failover within predeclared 15000 ms target: " + std::to_string(elapsed));
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
