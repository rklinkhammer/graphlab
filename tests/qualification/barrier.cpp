#define main m5_fixture_main
#include "../telemetry/linux.cpp"
#undef main
#include <cstring>
#include <graphlab/capture.hpp>
#include <linux/if_packet.h>
#include <net/if.h>
#include <pcap/pcap.h>

struct Sender {
  pid_t pid;
  int fd;
  std::string edge;
};
struct Barrier : runtime::Backend {
  runtime::LinuxBackend real;
  std::vector<Sender> senders;
  Json proof = Json::array();
  bool deny = false, released = false;
  ~Barrier() {
    for (auto &s : senders) {
      close(s.fd);
      kill(s.pid, SIGKILL);
      waitpid(s.pid, nullptr, 0);
    }
  }
  void configure(const std::filesystem::path &p) override { real.configure(p); }
  void preflight(const Json &t, const Json &a) override { real.preflight(t, a); }
  Json prepare(const Json &r, const Json &s) override { return real.prepare(r, s); }
  void remove(const Json &r, const Json &s) override { real.remove(r, s); }
  void activate(const Json &r) override { real.activate(r); }
  Json observe(const Json &r) override { return real.observe(r); }
  Json capture_plan(const Json &r) override { return real.capture_plan(r); }
  Json capture_control(const Json &r, const std::string &a) override {
    auto result = real.capture_control(r, a);
    if (a == "arm") {
      if (deny)
        throw runtime::Failure("test_arm_denied");
      for (const auto &c : r["captures"]) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
          throw std::runtime_error("sender pipe");
        auto pid = fork();
        if (pid < 0)
          throw std::runtime_error("sender fork");
        if (pid == 0) {
          close(pair[0]);
          int nsfd = open(c["namespacePath"].get<std::string>().c_str(), O_RDONLY);
          if (nsfd < 0 || setns(nsfd, CLONE_NEWNET))
            _exit(91);
          close(nsfd);
          int fd = socket(AF_PACKET, SOCK_RAW, htons(0x88b5));
          sockaddr_ll to{};
          to.sll_family = AF_PACKET;
          to.sll_ifindex = if_nametoindex(c["interface"].get<std::string>().c_str());
          to.sll_halen = 6;
          memset(to.sll_addr, 255, 6);
          if (fd < 0 || !to.sll_ifindex)
            _exit(92);
          char ready = 'r';
          write(pair[1], &ready, 1);
          if (read(pair[1], &ready, 1) != 1)
            _exit(93);
          unsigned char packet[128]{};
          memset(packet, 255, 6);
          packet[6] = 2;
          packet[12] = 0x88;
          packet[13] = 0xb5;
          auto marker = "M6-FIRST:" + c["edge"].get<std::string>() + ":0";
          memcpy(packet + 14, marker.data(), marker.size());
          auto timestamp = telemetry::monotonic();
          if (sendto(fd, packet, sizeof(packet), 0, (sockaddr *)&to, sizeof(to)) != sizeof(packet))
            _exit(94);
          write(pair[1], &timestamp, sizeof(timestamp));
          _exit(0);
        }
        close(pair[1]);
        senders.push_back({pid, pair[0], c["edge"]});
        pollfd p{pair[0], POLLIN, 0};
        char ready;
        if (poll(&p, 1, 3000) != 1 || read(pair[0], &ready, 1) != 1)
          throw std::runtime_error("sender readiness");
      }
    }
    return result;
  }
  void gate(const Json &r, const std::string &a) override {
    if (a != "release") {
      real.gate(r, a);
      return;
    }
    auto states = real.capture_control(r, "status");
    check(states.size() == r["captures"].size(), "every capture acknowledged before release");
    for (const auto &s : states)
      check(s["state"] == "active", "worker active before sender release");
    real.gate(r, a);
    released = true;
    auto start = telemetry::monotonic();
    for (auto &s : senders) {
      char go = 'g';
      if (write(s.fd, &go, 1) != 1)
        throw std::runtime_error("sender release");
    }
    for (auto &s : senders) {
      pollfd p{s.fd, POLLIN, 0};
      std::uint64_t sent;
      check(poll(&p, 1, 3000) == 1 && read(s.fd, &sent, sizeof(sent)) == sizeof(sent),
            "sender timestamp received");
      check(sent >= start && sent - start <= 250000000,
            "first numbered frame within 250ms of release");
      proof.push_back({{"edge", s.edge},
                       {"sequence", 0},
                       {"releaseNs", std::to_string(start)},
                       {"sentNs", std::to_string(sent)},
                       {"activation", states}});
      close(s.fd);
      int status;
      waitpid(s.pid, &status, 0);
      check(status == 0, "gated sender exited cleanly");
    }
    senders.clear();
  }
};
int main(int argc, char **argv) {
  if (argc != 3 || geteuid())
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    char tmp[] = "/tmp/gl6-bar-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    std::cout << "Evidence: " << root << std::endl;
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    check(runtime::process(
              {(bin / "m2_tests").string(), "--fixtures", argv[1], root.string(), argv[2]})
                  .code == 0,
          "fixtures generated");
    auto artifacts = console::load(root / "artifacts.lock.json");
    for (const std::string shape : {"chain", "star", "ring", "mesh", "disconnected", "isolated",
                                    "parallel", "triangle", "multi-nic"})
      for (bool deny : {false, true}) {
        auto folder = root / (shape + (deny ? "-denied" : "-released"));
        std::filesystem::create_directory(folder);
        auto t =
            (shape == "triangle" || shape == "multi-nic")
                ? console::load(root / "m2.yaml")
                : console::load(std::filesystem::path(argv[1]) / "topologies" / (shape + ".yaml"));
        t["artifactLock"] = lab_support::digest(artifacts);
        t["capture"]["required"] = true;
        if (shape == "multi-nic") {
          for (auto n : {"s1", "s2", "s3"})
            t["nodes"].erase(n);
          for (auto n : {"a", "b"})
            t["nodes"][n]["ports"]["data1"] = t["nodes"][n]["ports"]["data0"];
          t["edges"] =
              Json::array({{{"id", "first"}, {"endpoints", Json::array({"a:data0", "b:data0"})}},
                           {{"id", "second"}, {"endpoints", Json::array({"a:data1", "b:data1"})}}});
        }
        std::ofstream(folder / "topology.yaml") << t.dump();
        std::ofstream(folder / "artifacts.lock.json") << artifacts.dump();
        console::Catalog catalog(folder, folder / "artifacts.lock.json");
        auto valid = lab_support::validate(t, artifacts);
        check(bool(valid), shape + " validates");
        auto state = folder / "state";
        std::filesystem::create_directory(state);
        chmod(state.c_str(), 0700);
        Barrier backend;
        backend.deny = deny;
        runtime::Engine engine(state, backend, catalog);
        auto a = call(engine, "start",
                      {{"topologyHash", valid->hash}, {"idempotencyKey", "barrier-start"}});
        auto job = wait(engine, a);
        auto run = call(engine, "run", {{"id", a["runId"]}});
        std::string failure;
        try {
          if (deny)
            check(job["state"] == "failed" && !backend.released && backend.proof.empty(),
                  shape + " failed arm never opens sender gate");
          else {
            check(job["state"] == "succeeded", shape + " ready: " + job.dump());
            check(backend.proof.size() == t["edges"].size(), shape + " per-edge release proof");
            for (const auto &c : run["captures"]) {
              capture::worker_call(c, "rotate", run["controllerGeneration"]);
              auto dir = std::filesystem::path(c["directory"].get<std::string>());
              auto m = console::load(dir / "manifest.json");
              bool found = false;
              for (const auto &segment : m["segments"]) {
                char err[PCAP_ERRBUF_SIZE]{};
                auto *pcap =
                    pcap_open_offline((dir / segment["file"].get<std::string>()).c_str(), err);
                check(pcap != nullptr, "independent reader opens segment");
                pcap_pkthdr *h;
                const unsigned char *p;
                int result;
                while ((result = pcap_next_ex(pcap, &h, &p)) == 1)
                  found = found || std::string((const char *)p, h->caplen)
                                           .find("M6-FIRST:" + c["edge"].get<std::string>() +
                                                 ":0") != std::string::npos;
                pcap_close(pcap);
                check(result == -2, "clean independent EOF");
              }
              check(found, shape + " sequence zero captured at " + c["edge"].get<std::string>());
            }
          }
        } catch (const std::exception &e) {
          failure = e.what();
        }
        std::ofstream(folder / "release-proof.json") << backend.proof.dump(2);
        std::ofstream(folder / "run.json") << run.dump(2);
        auto cleanup = wait(engine, call(engine, "operate",
                                         {{"runId", run["id"]},
                                          {"expectedRevision", run["revision"]},
                                          {"operation", "recover"},
                                          {"idempotencyKey", "barrier-cleanup"}}));
        check(cleanup["state"] == "succeeded", shape + " cleanup");
        if (!failure.empty())
          throw std::runtime_error(failure);
      }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
