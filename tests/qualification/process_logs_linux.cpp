#include <fstream>
#include <graphlab/process_logs.hpp>
#include <graphlab/qemu.hpp>
#include <graphlab/terminal.hpp>
#include <iostream>
#include <sys/statfs.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
Json call(runtime::Engine &e, std::string m, Json p) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(runtime::Engine &e, Json a) {
  for (int i = 0; i < 600; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("job_timeout");
}
int main(int argc, char **argv) {
  try {
    if (argc < 3 || geteuid() != 0)
      return 2;
    std::filesystem::path root = argv[2];
    if (std::string(argv[1]) == "--full") {
      struct statfs fs{};
      check(!statfs(root.c_str(), &fs) && fs.f_type == 0x01021994 &&
                std::uint64_t(fs.f_blocks) * fs.f_bsize <= 16 * 1024 * 1024,
            "refuse exhaustion outside small dedicated tmpfs");
      process_logs::History history(root / "logs.sqlite");
      Json c = {{"runId", "full"}, {"node", "a"}, {"source", "docker-output"}};
      Json q = {{"runId", "full"}, {"node", "a"}};
      auto snap = [](std::string b) {
        return Json{{"base64", terminal::encode(b)},
                    {"generation", "g"},
                    {"observedAt", console::timestamp()}};
      };
      history.ingest(c, snap("retained"));
      auto before = history.query(q)["items"];
      {
        std::ofstream fill(root / "filler", std::ios::binary);
        std::string bytes(65536, 'x');
        while (fill)
          fill.write(bytes.data(), bytes.size());
      }
      bool failed = false;
      try {
        history.ingest(c, snap(std::string(65536, 'y')));
      } catch (const std::exception &e) {
        failed = true;
        std::cout << e.what() << std::endl;
      }
      check(failed, "real ENOSPC fails optional log publication");
      check(history.query(q)["items"] == before, "ENOSPC leaves committed artifact intact");
      std::filesystem::remove(root / "filler");
      history.ingest(c, snap(std::string(65536, 'y')));
      check(history.query(q)["items"].size() == 2,
            "retry after freeing space publishes complete artifact");
      return 0;
    }
    check(std::string(argv[1]) == "--qemu", "qualification mode");
    auto t = console::load(root / "topology.yaml"),
         lock = console::load(root / "artifacts.lock.json");
    if (argc == 4) {
      lock["workloads"]["guest-linux"]["vm"]["runnerImage"] = argv[3];
      t["artifactLock"] = lab_support::digest(lock);
      std::ofstream(root / "artifacts.lock.json") << lock.dump();
      std::ofstream(root / "topology.yaml") << t.dump();
    }
    console::Catalog catalog(root, root / "artifacts.lock.json");
    runtime::LinuxBackend backend;
    runtime::Engine e(root / "state", backend, catalog);
    auto a = call(e, "start",
                  {{"topologyHash", lab_support::validate(t, lock)->hash},
                   {"idempotencyKey", console::random_hex(16)}});
    auto job = wait(e, a);
    check(job["state"] == "succeeded", "real capture-first QEMU starts: " + job.dump());
    auto id = a["runId"];
    std::string artifact;
    try {
      auto run = call(e, "run", {{"id", id}});
      Json r;
      for (auto x : run["resources"])
        if (x["kind"] == "qemu")
          r = x;
      Json q = {{"runId", id}, {"node", "guest"}, {"source", "qemu-worker"}};
      Json page;
      for (int i = 0; i < 200; ++i) {
        page = call(e, "process-logs.query", q);
        if (!page["items"].empty())
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      check(!page["items"].empty(), "QEMU worker journal artifact collected");
      artifact = page["items"][0]["id"];
      auto data =
          call(e, "process-logs.download", {{"runId", id}, {"node", "guest"}, {"id", artifact}});
      auto bytes = terminal::decode(data["base64"]);
      check(bytes.find("Graphlab QEMU worker attached:") != std::string::npos,
            "nonempty actual worker journal diagnostic retained");
      auto invocation = data["generation"].get<std::string>().substr(
          data["generation"].get<std::string>().find(':') + 1);
      auto raw =
          runtime::process({"/usr/bin/journalctl", "--no-pager", "--quiet", "--output=short-iso",
                            "--lines=200", "--unit=" + r["identity"]["unit"].get<std::string>(),
                            "_SYSTEMD_INVOCATION_ID=" + invocation},
                           2);
      check(raw.code == 0 && raw.output == bytes,
            "QEMU artifact matches independent invocation-scoped journal bytes");
      if (argc == 4) {
        auto qr = q;
        qr["source"] = "qemu-runner";
        Json rp;
        for (int i = 0; i < 200; ++i) {
          rp = call(e, "process-logs.query", qr);
          if (!rp["items"].empty())
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(!rp["items"].empty(), "container-runner output retained separately");
        auto rd = call(e, "process-logs.download",
                       {{"runId", id}, {"node", "guest"}, {"id", rp["items"][0]["id"]}});
        auto c = runtime::docker_json(
            "GET", "/v1.52/containers/" + r["identity"]["runnerId"].get<std::string>() + "/json");
        auto rr = runtime::process({"/usr/bin/docker", "logs", "--timestamps", "--tail", "200",
                                    "--since", c["State"]["StartedAt"], c["Id"]},
                                   2);
        check(rr.code == 0 && rr.output == terminal::decode(rd["base64"]),
              "runner bytes independently match (empty output is valid)");
      }
      run = call(e, "run", {{"id", id}});
      check(wait(e, call(e, "operate",
                         {{"runId", id},
                          {"expectedRevision", run["revision"]},
                          {"operation", "destroy"},
                          {"idempotencyKey", console::random_hex(16)}}))["state"] == "succeeded",
            "owned QEMU run destroyed");
      check(call(e, "process-logs.download",
                 {{"runId", id}, {"node", "guest"}, {"id", artifact}})["base64"] == data["base64"],
            "QEMU log survives resource destruction");
    } catch (...) {
      auto run = call(e, "run", {{"id", id}});
      wait(e, call(e, "operate",
                   {{"runId", id},
                    {"expectedRevision", run["revision"]},
                    {"operation", run["state"] == "reconciling" ? "recover" : "destroy"},
                    {"idempotencyKey", console::random_hex(16)}}));
      throw;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
