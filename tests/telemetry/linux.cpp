#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <graphlab/telemetry.hpp>
#include <iostream>
#include <poll.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool b, std::string s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
Json call(runtime::Engine &e, std::string m, Json p) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, 0);
}
Json wait(runtime::Engine &e, Json a) {
  for (int i = 0; i < 1200; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "running" && j["state"] != "queued")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("job timeout");
}
void ns(int pid) {
  int fd = open(("/proc/" + std::to_string(pid) + "/ns/net").c_str(), O_RDONLY);
  if (fd < 0 || setns(fd, CLONE_NEWNET))
    _exit(90);
  close(fd);
}
sockaddr_in address(const char *s) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(49100);
  inet_pton(AF_INET, s, &a.sin_addr);
  return a;
}
struct Receiver {
  pid_t pid = -1;
  int control = -1;
  Receiver(int target, const char *ip) {
    int pair[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    pid = fork();
    if (pid == 0) {
      close(pair[0]);
      ns(target);
      int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
      auto a = address(ip);
      if (bind(fd, (sockaddr *)&a, sizeof(a)))
        _exit(91);
      unsigned count = 0;
      char ready = 'r';
      write(pair[1], &ready, 1);
      for (;;) {
        pollfd f[2] = {{fd, POLLIN, 0}, {pair[1], POLLIN, 0}};
        poll(f, 2, 100);
        if (f[0].revents & POLLIN) {
          char b[2048];
          while (recv(fd, b, sizeof(b), 0) > 0)
            ++count;
        }
        if (f[1].revents & POLLIN) {
          char c;
          if (read(pair[1], &c, 1) != 1)
            _exit(0);
          write(pair[1], &count, sizeof(count));
        }
        if (f[1].revents & POLLHUP)
          _exit(0);
      }
    }
    close(pair[1]);
    control = pair[0];
    pollfd p{control, POLLIN, 0};
    if (poll(&p, 1, 3000) != 1)
      throw std::runtime_error("receiver start");
    char r;
    read(control, &r, 1);
  }
  unsigned count() {
    char c = '?';
    write(control, &c, 1);
    unsigned n = 0;
    if (read(control, &n, sizeof(n)) != sizeof(n))
      throw std::runtime_error("receiver disconnected");
    return n;
  }
  ~Receiver() {
    close(control);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  }
};
void send(int target, const char *from, const char *to) {
  auto pid = fork();
  if (pid == 0) {
    ns(target);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    auto a = address(from);
    a.sin_port = 0;
    if (bind(fd, (sockaddr *)&a, sizeof(a)))
      _exit(92);
    auto b = address(to);
    char payload[256]{};
    for (int i = 0; i < 40; ++i) {
      if (sendto(fd, payload, sizeof(payload), 0, (sockaddr *)&b, sizeof(b)) != sizeof(payload))
        _exit(93);
      usleep(5000);
    }
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  check(status == 0, "owned namespace UDP sender");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
int main(int argc, char **argv) {
  if (argc != 3 || geteuid())
    return 2;
  int lockfd = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB))
    return 2;
  try {
    char tmp[] = "/tmp/gl5-live-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    std::cout << "Evidence: " << root << std::endl;
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    check(runtime::process(
              {(bin / "m2_tests").string(), "--fixtures", argv[1], root.string(), argv[2]})
                  .code == 0,
          "fixture generated");
    auto t = console::load(root / "m2.yaml"),
         artifacts = console::load(root / "artifacts.lock.json");
    for (auto key : {"s1", "s2", "s3"})
      t["nodes"].erase(key);
    t["nodes"]["a"]["ports"].erase("data1");
    t["nodes"]["b"]["ports"].erase("data1");
    t["edges"] =
        Json::array({{{"id", "wire"}, {"endpoints", Json::array({"a:data0", "b:data0"})}}});
    std::ofstream(root / "m2.yaml") << t.dump();
    console::Catalog catalog(root, root / "artifacts.lock.json");
    auto validated = lab_support::validate(t, artifacts);
    check(bool(validated), "direct edge validates");
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    runtime::LinuxBackend backend;
    Json run;
    std::string fid;
    {
      runtime::Engine e(state, backend, catalog);
      auto a = call(e, "start",
                    {{"topologyHash", validated->hash},
                     {"developmentMode", true},
                     {"idempotencyKey", "m5-live-start"}});
      auto j = wait(e, a);
      check(j["state"] == "succeeded", "start: " + j.dump());
      run = call(e, "run", {{"id", a["runId"]}});
      std::ofstream(root / "run.json") << run.dump();
      int apid = 0, bpid = 0;
      for (const auto &r : run["resources"])
        if (r["kind"] == "container") {
          auto c = runtime::docker_json(
              "GET", "/v1.52/containers/" + r["identity"]["id"].get<std::string>() + "/json");
          (r["logical"] == "a" ? apid : bpid) = c["State"]["Pid"];
        }
      Receiver receiverA(apid, "10.231.17.1"), receiverB(bpid, "10.231.17.2");
      auto before = backend.telemetry(run);
      check(before.size() == 1 && before[0]["valid"] == true,
            "one canonical stats64 source per edge: " + before.dump());
      send(apid, "10.231.17.1", "10.231.17.2");
      check(receiverB.count() == 40, "known A-to-B traffic delivered");
      auto after = backend.telemetry(run);
      auto delta = std::stoull(after[0]["raw"]["txPackets"].get<std::string>()) -
                   std::stoull(before[0]["raw"]["txPackets"].get<std::string>());
      check(delta >= 40 && delta <= 44 && after[0]["forwardMetric"] == "tx",
            "one-way canonical direction without endpoint doubling");
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      auto h = call(e, "telemetry", {{"runId", run["id"]}});
      check(!h["items"].empty(), "durable telemetry history available");
      auto apply = [&](std::string key, int duration) {
        run = call(e, "run", {{"id", run["id"]}});
        auto a = call(e, "fault.apply",
                      {{"runId", run["id"]},
                       {"expectedRevision", run["revision"]},
                       {"idempotencyKey", key},
                       {"fault",
                        {{"edge", "wire"},
                         {"kind", "netem"},
                         {"direction", "a-to-b"},
                         {"lossPercent", 100},
                         {"durationSeconds", duration}}}});
        auto result = wait(e, a);
        check(result["state"] == "succeeded", "netem apply: " + result.dump());
        return a["faultId"].get<std::string>();
      };
      fid = apply("m5-live-fault", 3);
      auto count = receiverB.count();
      send(apid, "10.231.17.1", "10.231.17.2");
      check(receiverB.count() == count, "netem blocks A-to-B");
      send(bpid, "10.231.17.2", "10.231.17.1");
      check(receiverA.count() == 40, "reverse B-to-A remains unaffected");
      std::this_thread::sleep_for(std::chrono::seconds(4));
      run = call(e, "run", {{"id", run["id"]}});
      check(run["faults"][fid]["state"] == "removed", "fault expires through serialized executor");
      send(apid, "10.231.17.1", "10.231.17.2");
      check(receiverB.count() == count + 40, "expiry restores packet delivery");
      fid = apply("m5-restart-fault", 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    {
      runtime::Engine e(state, backend, catalog);
      run = call(e, "run", {{"id", run["id"]}});
      check(run["faults"][fid]["state"] == "removed", "expired fault removed during restart");
      auto j = wait(e, call(e, "operate",
                            {{"runId", run["id"]},
                             {"expectedRevision", run["revision"]},
                             {"operation", "recover"},
                             {"idempotencyKey", "m5-live-cleanup"}}));
      check(j["state"] == "succeeded", "owned fault and run cleanup: " + j.dump());
    }
    std::cout << "M5 Linux tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
