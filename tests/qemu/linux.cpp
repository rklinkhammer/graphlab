#include <fcntl.h>
#include <fstream>
#include <graphlab/qemu.hpp>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
Json call(runtime::Engine &e, const std::string &m, Json p = Json::object()) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(runtime::Engine &e, Json a) {
  for (int i = 0; i < 1200; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("job timeout");
}
void write(const std::filesystem::path &p, const Json &j) { std::ofstream(p) << j.dump(2) << '\n'; }
struct Backend : runtime::Backend {
  runtime::LinuxBackend real;
  void configure(const std::filesystem::path &p) override { real.configure(p); }
  void preflight(const Json &t, const Json &a) override { real.preflight(t, a); }
  Json prepare(const Json &r, const Json &n) override {
    auto result = real.prepare(r, n);
    if (n["kind"] == "bridge") {
      auto name = runtime::resource_name(r, n["key"]);
      check(runtime::process({"/usr/bin/ovs-vsctl", "set", "Port", name, "tag=100"}).code == 0,
            "test-owned receiver VLAN");
      check(runtime::process({"/usr/sbin/ip", "address", "add", "10.233.17.1/24", "dev", name})
                    .code == 0,
            "test-owned guest traffic receiver");
    }
    return result;
  }
  void remove(const Json &r, const Json &n) override { real.remove(r, n); }
  void activate(const Json &r) override { real.activate(r); }
  void gate(const Json &r, const std::string &a) override { real.gate(r, a); }
  Json observe(const Json &r) override { return real.observe(r); }
  Json capture_plan(const Json &r) override { return real.capture_plan(r); }
  Json capture_control(const Json &r, const std::string &a) override {
    return real.capture_control(r, a);
  }
};
std::size_t guest_packets(const Json &run) {
  std::size_t count = 0;
  for (const auto &c : run["captures"])
    for (const auto &f : std::filesystem::directory_iterator(c["directory"].get<std::string>())) {
      if (f.path().extension() != ".pcapng" && f.path().extension() != ".partial")
        continue;
      std::ifstream in(f.path(), std::ios::binary);
      std::string b{std::istreambuf_iterator<char>(in), {}};
      for (std::size_t p = 0; (p = b.find("graphlab-guest-", p)) != std::string::npos; p += 14)
        ++count;
    }
  return count;
}
int main(int argc, char **argv) {
  if ((argc != 4 && !(argc == 5 && std::string(argv[4]) == "--fixtures")) || geteuid() != 0)
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    for (int profile = 0; profile < 2; ++profile) {
      char p[] = "/tmp/gl4-XXXXXX";
      auto root = std::filesystem::path(mkdtemp(p));
      auto state = root / "state";
      std::filesystem::create_directories(state / "artifacts");
      chmod(state.c_str(), 0700);
      std::cout << "Evidence: " << root << std::endl;
      auto artifacts =
          console::load(std::filesystem::path(argv[1]) / "topologies/artifacts.lock.json");
      auto guest = artifacts["workloads"]["guest-linux"];
      artifacts["workloads"] = Json::object();
      guest["platform"] = profile == 0 ? "linux/ppc64le" : "linux/arm64";
      guest["contract"]["platforms"] = Json::array({guest["platform"]});
      guest["contract"]["interfaces"].erase("data1");
      guest["contract"]["resources"]["memoryMiB"] = 512;
      guest["contractSha256"] = lab_support::digest(guest["contract"]);
      guest["vm"]["machine"] = profile == 0 ? "pseries-8.2" : "virt-8.2";
      guest["vm"]["accelerator"] = profile == 0 ? "tcg" : "kvm";
      auto input = std::filesystem::path(argv[profile + 2]);
      auto known = capture::file_hash(input / "known_hosts");
      guest["vm"]["sshUser"] = "root";
      guest["vm"]["knownHostsSha256"] = known;
      std::filesystem::copy_file(input / "known_hosts", state / "artifacts" / known.substr(7));
      chmod((state / "artifacts" / known.substr(7)).c_str(), 0400);
      std::filesystem::create_directory(state / "credentials");
      chmod((state / "credentials").c_str(), 0700);
      std::filesystem::copy_file(input / "client.key", state / "credentials/guest-linux.key");
      chmod((state / "credentials/guest-linux.key").c_str(), 0600);
      for (auto [file, key] : {std::pair{"disk.raw", "diskSha256"},
                               {"firmware", "firmwareSha256"},
                               {"kernel", "kernelSha256"},
                               {"initrd.gz", "initrdSha256"}}) {
        auto hash = capture::file_hash(input / file);
        if (std::string(file) == "disk.raw")
          guest[key] = hash;
        else
          guest["vm"][key] = hash;
        auto dest = state / "artifacts" / hash.substr(7);
        if (!std::filesystem::exists(dest))
          std::filesystem::copy_file(input / file, dest);
        chmod(dest.c_str(), 0400);
      }
      artifacts["workloads"]["guest-linux"] = guest;
      write(root / "artifacts.lock.json", artifacts);
      auto t = console::load(std::filesystem::path(argv[1]) / "topologies/isolated.yaml");
      t["id"] = profile == 0 ? "ppc64le-tcg" : "arm64-kvm";
      auto sw = t["nodes"]["s1"];
      t["nodes"] = Json::object();
      sw["ports"] = {
          {"p1",
           {{"role", "data"}, {"medium", "ethernet"}, {"mtu", 1500}, {"vlan", {{"access", 100}}}}}};
      t["nodes"]["s1"] = sw;
      t["nodes"]["guest"] = {
          {"kind", "qemu"},
          {"workload", "guest-linux"},
          {"ports", {{"data0", {{"role", "data"}, {"medium", "ethernet"}, {"mtu", 1500}}}}}};
      t["edges"] = Json::array(
          {{{"id", "guest-link"}, {"endpoints", Json::array({"guest:data0", "s1:p1"})}}});
      t["artifactLock"] = lab_support::digest(artifacts);
      t["nodes"]["guest"]["ports"]["mgmt0"] = {
          {"role", "management"}, {"medium", "ethernet"}, {"mtu", 1500}};
      t["management"]["networks"] = {{"control",
                                      {{"subnet", "172.31.243.0/24"},
                                       {"dynamicPool", "172.31.243.128/25"},
                                       {"externalAccess", false},
                                       {"gateway", "172.31.243.1"}}}};
      t["managementAttachments"] = Json::array(
          {{{"endpoint", "guest:mgmt0"}, {"network", "control"}, {"address", "172.31.243.10/24"}}});
      write(root / "topology.yaml", t);
      auto valid = lab_support::validate(t, artifacts);
      check(bool(valid), "guest topology validates");
      if (argc == 5)
        continue;
      console::Catalog catalog(root, root / "artifacts.lock.json");
      Backend backend;
      Json run, desc;
      bool data_ssh = false;
      {
        runtime::Engine e(state, backend, catalog);
        auto a = call(e, "start", {{"topologyHash", valid->hash}, {"idempotencyKey", "m4-start"}});
        auto j = wait(e, a);
        write(root / "start-job.json", j);
        check(j["state"] == "succeeded", "capture-first guest start: " + j.dump());
        run = call(e, "run", {{"id", a["runId"]}});
        write(root / "run.json", run);
        for (const auto &r : run["resources"])
          if (r["kind"] == "qemu")
            desc = r["identity"];
        check(!desc.is_null() && run["captures"].size() == 1, "guest TAP captured");
        bool ready = false;
        for (int i = 0; i < 1200; ++i) {
          auto s = terminal::request(desc, "status", run["controllerGeneration"]);
          if (s.value("guestReady", false)) {
            ready = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(ready, "guest readiness observed separately from process start");
        auto opened = call(e, "terminal",
                           {{"runId", run["id"]},
                            {"node", "guest"},
                            {"operation", "open"},
                            {"params", {{"recordInput", false}}}});
        auto sid = opened["id"];
        auto terminal = [&](std::string operation, Json params = Json::object()) {
          return call(e, "terminal",
                      {{"runId", run["id"]},
                       {"sessionId", sid},
                       {"operation", operation},
                       {"params", params}});
        };
        std::string ssh_output, seq = "0";
        for (int i = 0; i < 200; ++i) {
          auto page = terminal("replay", {{"sequence", seq}});
          seq = page["next"];
          for (const auto &r : page["records"])
            if (r["type"] == 1)
              ssh_output += terminal::decode(r["base64"]);
          if (ssh_output.find("GRAPHLAB_CONSOLE") != std::string::npos)
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(ssh_output.find("GRAPHLAB_CONSOLE") != std::string::npos,
              "guest SSH host key pinned and authenticated");
        auto scan = runtime::process(
            {"/usr/bin/ssh-keyscan", "-T", "3", "-t", "ed25519", "10.233.17.2"}, 10);
        data_ssh = scan.output.find("ssh-ed25519") != std::string::npos;
        auto access = terminal("acquire");
        terminal("resize", {{"token", access["token"]}, {"rows", 32}, {"columns", 90}});
        terminal("input",
                 {{"token", access["token"]}, {"base64", terminal::encode("echo M4_SSH_OK\n")}});
        for (int i = 0; i < 100; ++i) {
          auto page = terminal("replay", {{"sequence", seq}});
          seq = page["next"];
          for (const auto &r : page["records"])
            if (r["type"] == 1)
              ssh_output += terminal::decode(r["base64"]);
          if (ssh_output.find("M4_SSH_OK") != std::string::npos)
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(ssh_output.find("M4_SSH_OK") != std::string::npos,
              "guest SSH PTY input output and resize");
        terminal("close");
        auto writer =
            terminal::request(desc, "acquire", run["controllerGeneration"], {{"owner", "test"}});
        terminal::request(desc, "input", run["controllerGeneration"],
                          {{"owner", "test"},
                           {"token", writer["token"]},
                           {"base64", terminal::encode("echo m4-serial-test\n")}});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto replay = terminal::replay(
            std::filesystem::path(desc["directory"].get<std::string>()) / "output.partial", 0);
        check(!replay["records"].empty(), "serial output durably replayable");
        check(terminal::request(desc, "status", run["controllerGeneration"])["serialCoverage"] ==
                  "from-attachment",
              "first-byte claim explicitly unavailable");
      }
      auto before = terminal::request(desc, "status", run["controllerGeneration"]);
      std::this_thread::sleep_for(std::chrono::seconds(11));
      auto after = terminal::request(desc, "status", run["controllerGeneration"]);
      check(after["vmState"] == "paused",
            "independent watchdog pauses guest during controller outage");
      check(before["invocationId"] == after["invocationId"],
            "serial worker survives controller outage");
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto packets = guest_packets(run);
      check(packets > 0, "actual guest application packets captured");
      std::this_thread::sleep_for(std::chrono::seconds(2));
      check(guest_packets(run) == packets, "guest application traffic ceases after watchdog pause");
      if (profile == 1) {
        auto killed = runtime::process({"/usr/bin/systemctl", "kill", "--kill-whom=main",
                                        "--signal=SIGKILL", desc["unit"].get<std::string>()});
        check(killed.code == 0, "inject abrupt VM supervisor shutdown");
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      {
        runtime::Engine e(state, backend, catalog);
        run = call(e, "run", {{"id", run["id"]}});
        check(run["state"] == "reconciling", "restart holds guest for recovery");
        auto j = wait(e, call(e, "operate",
                              {{"runId", run["id"]},
                               {"expectedRevision", run["revision"]},
                               {"operation", "recover"},
                               {"idempotencyKey", "m4-recover"}}));
        check(j["state"] == "succeeded", "guest recovery cleanup: " + j.dump());
        auto retained = call(e, "run", {{"id", run["id"]}});
        bool exported = false;
        for (const auto &a : terminal::artifacts(retained)) {
          if (a.value("state", "") != "closed")
            continue;
          auto chunk = terminal::download(retained, a["id"], 0);
          check(terminal::decode(chunk["base64"]).starts_with(std::string("GLTERM1\0", 8)),
                "closed terminal download preserves recording header");
          exported = true;
        }
        check(exported, "retained run exports terminal recordings");
      }
      check(std::filesystem::exists(std::filesystem::path(desc["directory"].get<std::string>()) /
                                    (profile == 0 ? "output.glterm" : "output.partial")),
            profile == 0 ? "serial artifact finalized"
                         : "abrupt shutdown preserves partial recording");
      check(!data_ssh, "guest SSH is unavailable on the data interface");
      // A digest-valid but unloadable firmware blob must not leave running VM resources.
      auto bad = state / "bad-firmware";
      {
        std::ofstream image(bad);
        image.seekp(128 * 1024 * 1024);
        image.put(0);
      }
      auto badHash = capture::file_hash(bad);
      std::filesystem::rename(bad, state / "artifacts" / badHash.substr(7));
      chmod((state / "artifacts" / badHash.substr(7)).c_str(), 0400);
      artifacts["workloads"]["guest-linux"]["vm"]["firmwareSha256"] = badHash;
      t["artifactLock"] = lab_support::digest(artifacts);
      write(root / "artifacts.lock.json", artifacts);
      write(root / "topology.yaml", t);
      console::Catalog failureCatalog(root, root / "artifacts.lock.json");
      {
        runtime::Engine e(state, backend, failureCatalog);
        auto a = call(e, "start",
                      {{"topologyHash", lab_support::validate(t, artifacts)->hash},
                       {"idempotencyKey", "m4-invalid-firmware"}});
        auto j = wait(e, a);
        check(j["state"] == "failed", "invalid firmware fails guest start");
        auto failed = call(e, "run", {{"id", a["runId"]}});
        auto cleanup = wait(e, call(e, "operate",
                                    {{"runId", failed["id"]},
                                     {"expectedRevision", failed["revision"]},
                                     {"operation", "recover"},
                                     {"idempotencyKey", "m4-boot-cleanup"}}));
        check(cleanup["state"] == "succeeded", "failed-boot owned resource cleanup");
      }
    }
    std::cout << "M4 Linux QEMU tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
