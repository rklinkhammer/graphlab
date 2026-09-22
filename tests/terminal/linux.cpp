#include <fcntl.h>
#include <fstream>
#include <graphlab/terminal.hpp>
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
Json call(runtime::Engine &e, const std::string &m, Json p) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(runtime::Engine &e, Json a) {
  for (int i = 0; i < 900; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("timeout");
}
int main(int argc, char **argv) {
  if (argc != 3 || geteuid() != 0)
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    char p[] = "/tmp/gl4c-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(p));
    std::cout << "Evidence: " << root << std::endl;
    auto generator = std::filesystem::absolute(argv[0]).parent_path() / "m2_tests";
    check(runtime::process({generator.string(), "--fixtures", argv[1], root.string(), argv[2]})
                  .code == 0,
          "console fixture generated");
    console::Catalog catalog(root, root / "artifacts.lock.json");
    auto t = console::load(root / "m2.yaml");
    auto hash = lab_support::validate(t, console::load(root / "artifacts.lock.json"))->hash;
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    runtime::LinuxBackend backend;
    Json run;
    std::string session;
    {
      runtime::Engine e(state, backend, catalog);
      auto a = call(e, "start",
                    {{"topologyHash", hash},
                     {"idempotencyKey", "m4-console-start"},
                     {"developmentMode", true}});
      auto job = wait(e, a);
      check(job["state"] == "succeeded", "console run starts: " + job.dump());
      run = call(e, "run", {{"id", a["runId"]}});
      auto terminal = [&](std::string op, Json params = Json::object()) {
        return call(e, "terminal",
                    {{"runId", run["id"]},
                     {"node", "a"},
                     {"sessionId", session},
                     {"operation", op},
                     {"owner", "test"},
                     {"params", params}});
      };
      session = terminal("open")["id"];
      for (int i = 0; i < 100; ++i) {
        if (terminal("status").value("sourceReady", false))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      check(terminal("status").value("sourceReady", false),
            "scoped Docker exec FD attached without Docker socket in worker");
      auto lease = terminal("acquire");
      auto token = lease["token"];
      bool denied = false;
      try {
        terminal("acquire");
      } catch (...) {
        denied = true;
      }
      check(denied, "second writer denied");
      terminal("resize", {{"token", token}, {"rows", 33}, {"columns", 91}});
      terminal("input", {{"token", token},
                         {"base64", terminal::encode("printf 'M4_DOCKER_OK\\n'; stty size\n")}});
      std::string output, sequence = "0";
      bool has_input = false;
      for (int i = 0; i < 100; ++i) {
        auto records = terminal("replay", {{"sequence", sequence}});
        sequence = records["next"];
        for (const auto &r : records["records"]) {
          if (r["type"] == 1)
            output += terminal::decode(r["base64"]);
          has_input = has_input || r["type"] == 4;
        }
        if (output.find("33 91") != std::string::npos)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      check(output.find("M4_DOCKER_OK") != std::string::npos &&
                output.find("33 91") != std::string::npos,
            "real PTY input output and resize");
      check(!has_input, "default recording excludes input records");
      std::ofstream(root / "pty-output.txt") << output;
      auto next = terminal("acquire", {{"takeover", true}});
      denied = false;
      try {
        terminal("input", {{"token", token}, {"base64", terminal::encode("stale\n")}});
      } catch (...) {
        denied = true;
      }
      check(denied, "takeover fences old writer");
      terminal("input", {{"token", next["token"]},
                         {"base64", terminal::encode("sleep 1; printf 'SURVIVED_AGENT\\n'\n")}});
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
      runtime::Engine e(state, backend, catalog);
      run = call(e, "run", {{"id", run["id"]}});
      auto base = Json{{"runId", run["id"]},
                       {"sessionId", session},
                       {"owner", "test"},
                       {"params", Json::object()}};
      std::string seq = "0", out;
      for (int i = 0; i < 50; ++i) {
        base["operation"] = "replay";
        base["params"] = {{"sequence", seq}};
        auto r = call(e, "terminal", base);
        if (r["next"] == seq)
          break;
        seq = r["next"];
        for (const auto &v : r["records"])
          if (v["type"] == 1)
            out += terminal::decode(v["base64"]);
      }
      check(out.find("SURVIVED_AGENT") != std::string::npos,
            "output continues and replays through controller restart");
      auto descriptor = run["sessions"][0];
      auto exec = console::load(std::filesystem::path(descriptor["directory"].get<std::string>()) /
                                "exec.json");
      auto execpath = "/v1.52/exec/" + exec["id"].get<std::string>() + "/json";
      std::vector<std::string> end = {"/usr/bin/systemctl", "stop", descriptor["unit"]};
      if (getenv("GRAPHLAB_TEST_KILL_RECORDER"))
        end = {"/usr/bin/systemctl", "kill", "--kill-whom=main", "--signal=SIGKILL",
               descriptor["unit"]};
      check(runtime::process(end).code == 0, "terminate recorder independently");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto units = runtime::process(
          {"/usr/bin/systemctl", "list-units", "--all", "--no-legend", descriptor["unit"]});
      std::cout << "OBS recorder unit absent=" << units.output.empty() << std::endl;
      bool exited = false;
      for (int i = 0; i < 100; ++i) {
        if (runtime::docker_json("GET", execpath)["Running"] == false) {
          exited = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      // Model an exec left by an older recorder that had no exit hook.
      auto orphan = runtime::docker_json(
          "POST", "/v1.52/containers/" + descriptor["containerId"].get<std::string>() + "/exec",
          {{"Cmd", Json::array({"/bin/sleep", "120"})}});
      execpath = "/v1.52/exec/" + orphan["Id"].get<std::string>() + "/json";
      capture::atomic_json(std::filesystem::path(descriptor["directory"].get<std::string>()) /
                               "exec.json",
                           {{"id", orphan["Id"]}, {"containerId", descriptor["containerId"]}});
      runtime::docker_json("POST", "/v1.52/exec/" + orphan["Id"].get<std::string>() + "/start",
                           {{"Detach", true}, {"Tty", false}});
      bool orphan_running = runtime::docker_json("GET", execpath)["Running"];
      if (!units.output.empty())
        runtime::process({"/usr/bin/systemctl", "reset-failed", descriptor["unit"]});
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      bool missing_unit = runtime::process({"/usr/bin/systemctl", "list-units", "--all",
                                            "--no-legend", descriptor["unit"]})
                              .output.empty();
      base["operation"] = "close";

      base["params"] = Json::object();
      call(e, "terminal", base);
      call(e, "terminal", base);
      bool workload_alive = runtime::docker_json(
          "GET", "/v1.52/containers/" + descriptor["containerId"].get<std::string>() +
                     "/json")["State"]["Running"];
      bool leaked = runtime::docker_json("GET", execpath)["Running"];
      std::cout << "OBS exec running after successful close=" << leaked << std::endl;

      auto job = wait(e, call(e, "operate",
                              {{"runId", run["id"]},
                               {"expectedRevision", run["revision"]},
                               {"operation", "recover"},
                               {"idempotencyKey", "m4-console-cleanup"}}));
      check(job["state"] == "succeeded", "console run cleanup: " + job.dump());
      check(exited, "recorder exit hook terminates Docker exec before explicit close");
      check(missing_unit, "explicit close cleans orphan exec with absent recorder unit");
      check(orphan_running, "orphan fixture was running before explicit close");
      check(workload_alive, "exec cleanup preserves parent workload");
      check(!leaked, "closed recorder must not leave Docker exec alive");
    }
    std::cout << "M4 Docker console tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
