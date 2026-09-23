// Real Docker PTYs: privacy, explicit quota failure, retained prefix and cleanup.
#define main existing_terminal_main
#include "../terminal/linux.cpp"
#undef main
Json records(const std::filesystem::path &path) {
  Json out = Json::array();
  std::uint64_t seq = 0, offset = 0;
  for (;;) {
    auto page = terminal::replay(path, seq);
    check(!page["partialTail"].get<bool>(), "complete retained record boundary");
    if (page["records"].empty())
      break;
    std::size_t pageBytes = 0;
    for (const auto &r : page["records"]) {
      auto bytes = terminal::decode(r["base64"]);
      pageBytes += bytes.size();
      check(r["sequence"] == std::to_string(seq++) && r["offset"] == std::to_string(offset),
            "exact record sequence/output offset");
      if (r["type"] == 1)
        offset += bytes.size();
      out.push_back(r);
    }
    check(pageBytes <= 65536 && page["records"].size() <= 128, "bounded replay page");
  }
  return out;
}
std::string output(const Json &records) {
  std::string s;
  for (const auto &r : records)
    if (r["type"] == 1)
      s += terminal::decode(r["base64"]);
  return s;
}
int main(int argc, char **argv) {
  if (argc != 3 || geteuid())
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    char tmp[] = "/tmp/gl6-pty-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(tmp));
    std::cout << "Evidence: " << root << std::endl;
    auto bin = std::filesystem::absolute(argv[0]).parent_path();
    check(runtime::process(
              {(bin / "m2_tests").string(), "--fixtures", argv[1], root.string(), argv[2]})
                  .code == 0,
          "live terminal fixture");
    auto t = console::load(root / "m2.yaml"),
         artifacts = console::load(root / "artifacts.lock.json");
    auto hash = lab_support::validate(t, artifacts)->hash;
    auto state = root / "state";
    std::filesystem::create_directory(state);
    chmod(state.c_str(), 0700);
    console::Catalog catalog(root, root / "artifacts.lock.json");
    runtime::LinuxBackend backend;
    runtime::Engine e(state, backend, catalog);
    auto started = call(e, "start",
                        {{"topologyHash", hash},
                         {"developmentMode", true},
                         {"idempotencyKey", "terminal-live-start"}});
    check(wait(e, started)["state"] == "succeeded", "live terminal run starts");
    auto run = call(e, "run", {{"id", started["runId"]}});
    Json resource;
    for (const auto &r : run["resources"])
      if (r["key"] == "node/a")
        resource = r;
    Json evidence = Json::array();
    for (int mode = 0; mode < 3; ++mode) {
      auto d = terminal::docker_session(run, resource, state / "sessions", mode == 1);
      d["byteBudget"] = mode == 2 ? 65536 : 1048576;
      terminal::launch(d);
      terminal::docker_attach(d, run["controllerGeneration"]);
      auto dir = std::filesystem::path(d["directory"].get<std::string>());
      auto request = [&](std::string op, Json p = Json::object()) {
        p["owner"] = "qualification";
        return terminal::request(d, op, run["controllerGeneration"], p);
      };
      for (int i = 0; i < 100 && !request("status").value("sourceReady", false); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      check(request("status").value("sourceReady", false), "real exec stream attached");
      auto token = request("acquire")["token"];
      bool denied = false;
      try {
        request("acquire");
      } catch (...) {
        denied = true;
      }
      check(denied, "second writer refused");
      request("resize", {{"token", token}, {"rows", 42}, {"columns", 111}});
      terminal::docker_resize(d, 42, 111);
      auto send = [&](std::string b) {
        request("input", {{"token", token}, {"base64", terminal::encode(b)}});
      };
      send("stty -echo; printf 'PRIVACY_READY\\n'; read secret; printf 'INPUT_DONE\\n'\n");
      auto until = [&](std::string marker) {
        for (int i = 0; i < 200; ++i) {
          std::string seq = "0", s;
          for (;;) {
            auto p = terminal::replay(dir / "output.partial", std::stoull(seq));
            if (p["next"] == seq)
              break;
            seq = p["next"];
            s += output(p["records"]);
          }
          if (s.find(marker) != std::string::npos)
            return;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error("PTY marker timeout: " + marker);
      };
      // The echoed setup contains the marker too; require a completed line followed by
      // a quiet interval before sending the secret to the blocking read.
      until("PRIVACY_READY\r\n");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::string secret = "M6_PRIVATE_" + console::random_hex(12) + "\n";
      send(secret);
      until("INPUT_DONE\r\n");
      if (mode == 2) {
        send("head -c 262144 /dev/zero | tr '\\000' Q; printf 'QUOTA_END\\n'\n");
        Json manifest;
        for (int i = 0; i < 300; ++i) {
          manifest = console::load(dir / "manifest.json");
          if (manifest.value("state", "") == "incomplete")
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(manifest["state"] == "incomplete" && manifest["error"] == "recording_quota_exhausted",
              "quota is an explicit recording discontinuity");
        check(std::filesystem::file_size(dir / "output.partial") <= 65536 &&
                  !std::filesystem::exists(dir / "output.glterm"),
              "quota retains bounded prefix without publishing complete artifact");
        auto retained = records(dir / "output.partial");
        auto text = output(retained);
        check(text.find("PRIVACY_READY") != std::string::npos &&
                  text.find("QUOTA_END") == std::string::npos,
              "earliest output retained, later output not falsely claimed");
        auto digest = capture::file_hash(dir / "output.partial");
        terminal::stop(d, run["controllerGeneration"]);
        terminal::stop(d, run["controllerGeneration"]);
        check(capture::file_hash(dir / "output.partial") == digest,
              "recovery preserves partial bytes");
        Json view = run;
        view["sessions"] = Json::array({d});
        auto a = terminal::artifacts(view);
        check(a[0]["state"] == "partial", "artifact API explicitly exposes incomplete recording");
        evidence.push_back({{"mode", "quota"},
                            {"manifest", manifest},
                            {"artifact", a},
                            {"sha256", digest},
                            {"descriptor", d}});
      } else {
        send("stty size; printf 'MODE_DONE\\n'\n");
        until("MODE_DONE\r\n");
        terminal::stop(d, run["controllerGeneration"]);
        auto data = records(dir / "output.glterm");
        bool resize = false, input = false, secretSeen = false;
        for (const auto &r : data) {
          auto b = terminal::decode(r["base64"]);
          resize = resize || (r["type"] == 2 && Json::parse(b)["rows"] == 42);
          input = input || r["type"] == 4;
          secretSeen = secretSeen || b.find(secret) != std::string::npos;
        }
        check(resize && output(data).find("42 111") != std::string::npos,
              "resize event retained and applied to real PTY");
        check(input == (mode == 1) && secretSeen == (mode == 1),
              "default excludes secret bytes; opt-in retains exact input");
        auto second = records(dir / "output.glterm");
        check(second == data, "independent late viewer exact replay");
        evidence.push_back({{"mode", mode == 1 ? "opt-in" : "default"},
                            {"descriptor", d},
                            {"sha256", capture::file_hash(dir / "output.glterm")},
                            {"records", data.size()}});
      }
    }
    run = call(e, "run", {{"id", run["id"]}});
    check(wait(e, call(e, "operate",
                       {{"runId", run["id"]},
                        {"expectedRevision", run["revision"]},
                        {"operation", "recover"},
                        {"idempotencyKey", "terminal-live-cleanup"}}))["state"] == "succeeded",
          "scoped terminal fixture cleanup");
    std::ofstream(root / "proof.json") << evidence.dump(2);
    std::cout << "T12 live privacy/quota passed\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    return 1;
  }
}
