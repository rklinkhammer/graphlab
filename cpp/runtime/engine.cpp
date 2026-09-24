#include <fcntl.h>
#include <graphlab/application_telemetry.hpp>
#include <graphlab/capture.hpp>
#include <graphlab/packet_history.hpp>
#include <graphlab/runtime.hpp>
#include <graphlab/telemetry.hpp>
#include <graphlab/terminal.hpp>
#include <regex>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace graphlab::runtime {
namespace {
void sql(sqlite3 *db, const char *statement) {
  if (sqlite3_exec(db, statement, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw Failure("database_error", 503);
}
std::string uuid() {
  auto s = console::random_hex(16);
  s[12] = '4';
  s[16] = '8';
  return s.substr(0, 8) + "-" + s.substr(8, 4) + "-" + s.substr(12, 4) + "-" + s.substr(16, 4) +
         "-" + s.substr(20);
}
void fields(const Json &j, std::initializer_list<std::string_view> allowed) {
  if (!j.is_object())
    throw Failure("invalid_params");
  for (const auto &[k, v] : j.items()) {
    (void)v;
    if (std::find(allowed.begin(), allowed.end(), k) == allowed.end())
      throw Failure("unknown_parameter");
  }
}
std::string string(const Json &j, const std::string &key) {
  if (!j.contains(key) || !j[key].is_string())
    throw Failure("missing_" + key);
  return j[key];
}
bool pending(const Json &j) { return j["state"] == "queued" || j["state"] == "running"; }
} // namespace
std::string resource_name(const Json &run, const std::string &key) {
  auto h = lab_support::digest(Json::array({run["id"], key}));
  return "gl" + h.substr(7, 12);
}
std::vector<Json> resources(const Json &run) {
  std::vector<Json> result;
  const auto &t = run["topology"];
  for (const auto &[id, n] : t["management"]["networks"].items())
    result.push_back(
        {{"key", "management/" + id}, {"kind", "network"}, {"logical", id}, {"configuration", n}});
  for (const auto &[id, n] : t["nodes"].items())
    result.push_back({{"key", "node/" + id},
                      {"kind", n["kind"] == "ovs-switch" ? "bridge"
                               : n["kind"] == "qemu"     ? "qemu"
                                                         : "container"},
                      {"logical", id},
                      {"configuration", n}});
  for (const auto &e : t["edges"])
    result.push_back({{"key", "edge/" + e["id"].get<std::string>()},
                      {"kind", "edge"},
                      {"logical", e["id"]},
                      {"configuration", e}});
  return result;
}
Engine::Engine(const std::filesystem::path &directory, Backend &backend,
               const console::Catalog &catalog)
    : backend_(backend), catalog_(catalog), directory_(directory) {
  struct stat info{};
  if (lstat(directory.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0077))
    throw Failure("state_directory_requires_0700");
  backend_.configure(directory);
  lock_ = open((directory / "agent.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_ < 0 || flock(lock_, LOCK_EX | LOCK_NB)) {
    if (lock_ >= 0)
      close(lock_);
    lock_ = -1;
    throw Failure("agent_already_running", 409);
  }
  try {
    auto path = directory / "state.sqlite";
    if (std::filesystem::is_symlink(path))
      throw Failure("unsafe_database_path");
    if (sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
      throw Failure("database_open");
    chmod(path.c_str(), 0600);
    sql(db_,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; CREATE TABLE IF "
        "NOT EXISTS state (id INTEGER PRIMARY KEY CHECK(id=1), document TEXT NOT NULL);");
    telemetry::initialize(db_);
    application_telemetry::initialize(db_);
    // Optional derived index failure must not prevent capture or legacy execution.
    try {
      packet_history_ = std::make_unique<packets::History>(directory_ / "packet-history.sqlite");
    } catch (const std::exception &) {
    }
    sqlite3_stmt *query = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT document FROM state WHERE id=1", -1, &query, nullptr) !=
        SQLITE_OK)
      throw Failure("database_read");
    if (sqlite3_step(query) == SQLITE_ROW) {
      auto text = reinterpret_cast<const char *>(sqlite3_column_text(query, 0));
      try {
        state_ = Json::parse(text);
      } catch (...) {
        sqlite3_finalize(query);
        throw;
      }
    } else
      state_ = {{"apiVersion", "graphlab.state/v1"},
                {"runs", Json::object()},
                {"jobs", Json::object()},
                {"keys", Json::object()}};
    sqlite3_finalize(query);
    if (state_.value("apiVersion", "") != "graphlab.state/v1")
      throw Failure("unsupported_database");
    for (auto &[id, job] : state_["jobs"].items())
      if (pending(job)) {
        job["state"] = "failed";
        job["error"] = "agent_interrupted";
        job["finishedAt"] = console::timestamp();
        state_["runs"][job["runId"].get<std::string>()]["state"] = "reconciling";
      }
    save();
    for (auto &[id, run] : state_["runs"].items()) {
      bool has_vm = std::any_of(run["resources"].begin(), run["resources"].end(),
                                [](const Json &r) { return r["kind"] == "qemu"; });
      if (run["state"] == "destroyed" || (run["topology"]["capture"]["required"] != true &&
                                          !has_vm && run.value("sessions", Json::array()).empty()))
        continue;
      bool stopped = run["state"] == "stopped";
      auto coverage = run.value("captureCoverage", "pending");
      bool closed = coverage.starts_with("closed");
      run["controllerGeneration"] =
          std::to_string(std::stoull(run.value("controllerGeneration", "1")) + 1);
      run["state"] = "reconciling";
      if (!closed && coverage != "incomplete")
        run["captureCoverage"] = "interrupted-controller";
      save();
      try {
        if (!closed)
          run["captureObservations"] = backend_.capture_control(run, "adopt");
        backend_.gate(run, "quiesce");
        for (const auto &s : run.value("sessions", Json::array())) {
          try {
            terminal::request(s, "adopt", run["controllerGeneration"]);
          } catch (...) {
            run["terminalRecoveryRequired"] = true;
          }
        }
        if (stopped && closed)
          run["state"] = "stopped";
        run[closed ? "reconciledAt" : "adoptedAt"] = console::timestamp();
        // Adoption never silently renews traffic or changes the current capture epoch.
      } catch (const std::exception &e) {
        run["captureError"] = e.what();
        run["captureCoverage"] = "incomplete";
      }
      save();
    }
    m5_recover();
    worker_ = std::thread([this] { work(); });
  } catch (...) {
    if (db_)
      sqlite3_close(db_);
    close(lock_);
    throw;
  }
}
Engine::~Engine() {
  {
    std::lock_guard guard(mutex_);
    stopping_ = true;
    condition_.notify_all();
  }
  if (worker_.joinable())
    worker_.join();
  packet_history_.reset();
  sqlite3_close(db_);
  if (lock_ >= 0)
    close(lock_);
}
void Engine::save() {
  try {
    auto data = state_.dump();
    if (data.size() > 16 * 1024 * 1024)
      throw Failure("journal_capacity", 429);
    sql(db_, "BEGIN IMMEDIATE");
    sqlite3_stmt *statement = nullptr;
    int result = sqlite3_prepare_v2(
        db_,
        "INSERT INTO state VALUES(1,?) ON CONFLICT(id) DO UPDATE SET document=excluded.document",
        -1, &statement, nullptr);
    if (result == SQLITE_OK)
      result = sqlite3_bind_text(statement, 1, data.c_str(), static_cast<int>(data.size()),
                                 SQLITE_TRANSIENT);
    if (result == SQLITE_OK)
      result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
      sql(db_, "ROLLBACK");
      throw Failure("journal_commit_failed", 503);
    }
    sql(db_, "COMMIT");
  } catch (...) {
    stopping_ = true;
    condition_.notify_all();
    throw;
  }
}
Json Engine::dispatch(const Json &request, uid_t principal) {
  fields(request, {"apiVersion", "method", "params"});
  if (request.value("apiVersion", "") != "graphlab.rpc/v1")
    throw Failure("unsupported_protocol");
  auto method = string(request, "method");
  if (!request.contains("params") || !request["params"].is_object())
    throw Failure("invalid_params");
  const auto &p = request["params"];
  std::unique_lock guard(mutex_);
  if (stopping_)
    throw Failure("journal_unavailable", 503);
  if (auto m5 = m5_dispatch(request, principal))
    return *m5;
  if (method == "terminal") {
    fields(p, {"runId", "node", "sessionId", "operation", "params", "owner"});
    auto runid = string(p, "runId");
    if (!state_["runs"].contains(runid))
      throw Failure("not_found", 404);
    auto &run = state_["runs"][runid];
    auto operation = string(p, "operation");
    auto args = p.value("params", Json::object());
    const auto owner = std::to_string(principal) + ":" + p.value("owner", std::string("cli"));
    args["owner"] = owner;
    if (operation == "logs") {
      fields(p, {"runId", "node", "operation", "owner"});
      auto node = string(p, "node");
      for (const auto &resource : run["resources"])
        if (resource["logical"] == node &&
            (resource["kind"] == "container" || resource["kind"] == "qemu")) {
          if (!resource.contains("identity"))
            throw Failure("node_not_started", 409);
          return backend_.logs(run, resource);
        }
      throw Failure("node_logs_unavailable", 404);
    }
    if (operation == "list") {
      Json items = Json::array();
      for (const auto &r : run["resources"])
        if (r["kind"] == "qemu" && r.contains("identity"))
          items.push_back(
              {{"id", r["identity"]["id"]}, {"node", r["logical"]}, {"kind", "serial"}});
      for (const auto &s : run.value("sessions", Json::array()))
        items.push_back({{"id", s["id"]},
                         {"node", s["node"]},
                         {"kind", s["kind"]},
                         {"recordInput", s.value("recordInput", false)}});
      return {{"items", items}};
    }
    if (operation == "open") {
      if (run["state"] != "ready" && run["state"] != "stopped")
        throw Failure("terminal_run_not_available", 409);
      for (const auto &[id, j] : state_["jobs"].items())
        if (pending(j))
          throw Failure("operation_in_progress", 409);
      if (!run.contains("sessions"))
        run["sessions"] = Json::array();
      if (run["sessions"].size() >= 16)
        throw Failure("terminal_session_limit");
      auto node = string(p, "node");
      Json resource;
      for (const auto &r : run["resources"])
        if (r["logical"] == node && (r["kind"] == "container" || r["kind"] == "qemu"))
          resource = r;
      if (resource.is_null())
        throw Failure("terminal_capability_unavailable");
      auto d =
          resource["kind"] == "qemu"
              ? terminal::ssh_session(run, resource, directory_, args.value("recordInput", false))
              : terminal::docker_session(run, resource, directory_ / "sessions",
                                         args.value("recordInput", false));
      run["sessions"].push_back(d);
      save();
      detail::checkpoint("terminal.intent");
      try {
        terminal::launch(d);
        detail::checkpoint("terminal.worker");
        if (d["kind"] == "docker")
          terminal::docker_attach(d, run["controllerGeneration"]);
        bool ready = false;
        for (int i = 0; i < 100; ++i) {
          try {
            auto status = terminal::request(d, "status", run["controllerGeneration"]);
            if (status["state"] == "active") {
              ready = true;
              break;
            }
          } catch (...) {
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!ready)
          throw Failure("terminal_worker_not_ready");
        detail::checkpoint("terminal.readback");
      } catch (...) {
        try {
          terminal::stop(d, run["controllerGeneration"]);
        } catch (...) {
          run["terminalRecoveryRequired"] = true;
          save();
        }
        throw;
      }
      detail::checkpoint("terminal.completion");
      return {{"id", d["id"]}, {"recordInput", d["recordInput"]}};
    }
    Json d;
    auto sid = string(p, "sessionId");
    for (const auto &s : run.value("sessions", Json::array()))
      if (s["id"] == sid)
        d = s;
    for (const auto &r : run["resources"])
      if (r["kind"] == "qemu" && r.contains("identity") && r["identity"]["id"] == sid)
        d = r["identity"];
    if (d.is_null())
      throw Failure("session_not_found", 404);
    auto dir = std::filesystem::path(d["directory"].get<std::string>());
    if (operation == "replay") {
      auto seq = args.value("sequence", std::string("0"));
      if (seq.empty() || seq.size() > 16 ||
          seq.find_first_not_of("0123456789") != std::string::npos)
        throw Failure("invalid_replay_sequence");
      guard.unlock();
      return terminal::replay(dir / (std::filesystem::exists(dir / "output.glterm")
                                         ? "output.glterm"
                                         : "output.partial"),
                              std::stoull(seq));
    }
    if (operation != "status" && operation != "acquire" && operation != "renew-writer" &&
        operation != "input" && operation != "resize" && operation != "revoke" &&
        operation != "close" && operation != "release-writer")
      throw Failure("unsupported_terminal_operation");
    if (operation == "close") {
      if (d["kind"] == "qemu")
        throw Failure("serial_lifetime_is_run");
      terminal::stop(d, run["controllerGeneration"]);
      return Json::object();
    }
    auto result = terminal::request(d, operation, run["controllerGeneration"], args);
    if (operation == "resize" && d["kind"] == "docker")
      terminal::docker_resize(d, args.at("rows"), args.at("columns"));
    return result;
  }
  if (method == "runs") {
    fields(p, {});
    Json runs = Json::array();
    for (const auto &[id, r] : state_["runs"].items())
      runs.push_back({{"id", id},
                      {"state", r["state"]},
                      {"revision", r["revision"]},
                      {"topologyHash", r["topologyHash"]}});
    return {{"items", runs}};
  }
  if (method == "packet-history" || method == "packet-history.rebuild" ||
      method == "packet-history.recover") {
    auto id = string(p, "runId");
    if (!state_["runs"].contains(id))
      throw Failure("not_found", 404);
    if (method == "packet-history.recover") {
      fields(p, {"runId", "scope"});
      if (p.value("scope", "") != "all-runs")
        throw Failure("packet_recovery_requires_all_runs_scope");
      for (const auto &[key, run] : state_["runs"].items())
        if (run["state"] != "stopped" && run["state"] != "destroyed")
          throw Failure("packet_recovery_requires_quiescence", 409);
      packet_history_.reset();
      auto path = directory_ / "packet-history.sqlite";
      try {
        packets::History::recover(path);
        packet_history_ = std::make_unique<packets::History>(path);
      } catch (...) {
        try {
          packet_history_ = std::make_unique<packets::History>(path);
        } catch (...) {
        }
        throw;
      }
      return {{"operation", "recover"}, {"state", "completed"}, {"scope", "all-runs"}};
    }
    if (!packet_history_)
      throw Failure("packet_history_unavailable", 503);
    if (method == "packet-history.rebuild") {
      fields(p, {"runId"});
      return packet_history_->rebuild(state_["runs"][id]);
    }
    return packet_history_->query(state_["runs"][id], p);
  }
  if (method == "artifacts" || method == "artifact") {
    fields(p, method == "artifacts"
                  ? std::initializer_list<std::string_view>{"runId"}
                  : std::initializer_list<std::string_view>{"runId", "id", "offset"});
    auto id = string(p, "runId");
    if (!state_["runs"].contains(id))
      throw Failure("not_found", 404);
    auto run = state_["runs"][id];
    guard.unlock();
    if (method == "artifacts")
      return capture::artifacts(run);
    auto offset = string(p, "offset");
    if (offset.empty() || offset.size() > 16 ||
        offset.find_first_not_of("0123456789") != std::string::npos)
      throw Failure("invalid_offset");
    return capture::download(run, string(p, "id"), std::stoull(offset));
  }
  if (method == "run" || method == "job") {
    fields(p, {"id"});
    auto id = string(p, "id");
    auto &collection = state_[method == "run" ? "runs" : "jobs"];
    if (!collection.contains(id))
      throw Failure("not_found", 404);
    return collection[id];
  }
  if (method == "cancel") {
    fields(p, {"id"});
    auto id = string(p, "id");
    if (!state_["jobs"].contains(id))
      throw Failure("not_found", 404);
    auto &j = state_["jobs"][id];
    if (!pending(j))
      return j;
    if (j["operation"] != "start" && j["operation"] != "fault.apply")
      throw Failure("cleanup_operations_not_cancellable", 409);
    j["cancelRequested"] = true;
    save();
    condition_.notify_all();
    return j;
  }
  if (method != "start" && method != "operate")
    throw Failure("unsupported_method");
  fields(p, method == "start"
                ? std::initializer_list<std::string_view>{"topologyHash", "idempotencyKey",
                                                          "developmentMode", "capturePolicy",
                                                          "observationProfile"}
                : std::initializer_list<std::string_view>{"runId", "operation", "expectedRevision",
                                                          "idempotencyKey"});
  auto key = string(p, "idempotencyKey");
  static const std::regex pattern("[A-Za-z0-9_-]{8,80}");
  if (!std::regex_match(key, pattern))
    throw Failure("invalid_idempotency_key");
  key = std::to_string(principal) + ":" + key;
  auto fingerprint = lab_support::digest(request);
  if (state_["keys"].contains(key)) {
    const auto &old = state_["keys"][key];
    if (old["hash"] != fingerprint)
      throw Failure("idempotency_conflict", 409);
    auto job = state_["jobs"][old["jobId"].get<std::string>()];
    return {{"jobId", job["id"]}, {"runId", job["runId"]}, {"revision", job["revision"]}};
  }
  if (stopping_)
    throw Failure("agent_stopping", 503);
  if (state_["jobs"].size() >= 256)
    throw Failure("journal_job_capacity", 429);
  for (const auto &[id, job] : state_["jobs"].items())
    if (pending(job))
      throw Failure("operation_in_progress", 409);
  std::string id, operation;
  Json run;
  if (method == "start") {
    const auto profile = p.value("observationProfile", std::string("full"));
    if (profile != "full" && profile != "minimal")
      throw Failure("invalid_observation_profile");
    for (const auto &[other, r] : state_["runs"].items())
      if (r["state"] != "destroyed")
        throw Failure("active_run_exists", 409);
    auto hash = string(p, "topologyHash");
    Json resolved;
    try {
      resolved = catalog_.resolve(hash);
    } catch (...) {
      throw Failure("unknown_topology", 404);
    }
    auto &t = resolved["topology"];
    if (t["capture"]["required"] == false && p.value("developmentMode", false) != true)
      throw Failure("development_mode_required");
    Json policy = {{"runBytes", 6ull * 1024 * 1024 * 1024},
                   {"reserveBytes", 5ull * 1024 * 1024 * 1024},
                   {"rotateBytes", 64ull * 1024 * 1024},
                   {"rotateSeconds", 60}};
    if (p.contains("capturePolicy")) {
      fields(p["capturePolicy"], {"runBytes", "reserveBytes", "rotateBytes", "rotateSeconds"});
      for (const auto &[k, v] : p["capturePolicy"].items()) {
        if (!v.is_number_unsigned() && (!v.is_number_integer() || v.get<std::int64_t>() < 0))
          throw Failure("invalid_capture_policy");
        policy[k] = v;
      }
    }
    if (policy["runBytes"] < 262144 || policy["runBytes"] > 6ull * 1024 * 1024 * 1024 ||
        policy["reserveBytes"] < 1024 * 1024 ||
        policy["reserveBytes"] > 5ull * 1024 * 1024 * 1024 || policy["rotateBytes"] < 4096 ||
        policy["rotateBytes"] > 64ull * 1024 * 1024 || policy["rotateSeconds"] < 1 ||
        policy["rotateSeconds"] > 60)
      throw Failure("invalid_capture_policy");
    for (const auto &[name, node] : t["nodes"].items()) {
      if (t["capture"]["required"] == true && node["kind"] == "docker" &&
          resolved["artifacts"]["workloads"][node["workload"].get<std::string>()]["contract"]
                  ["lifecycle"]["quiesce"] != "supported")
        throw Failure("capture_requires_reversible_quiescence");
      for (const auto &[port, v] : node["ports"].items())
        if (port.size() > 15)
          throw Failure("linux_interface_name_too_long");
    }
    id = uuid();
    operation = "start";
    run = {{"id", id},
           {"observationProfile", profile},
           {"generation", "1"},
           {"revision", "1"},
           {"state", "preparing"},
           {"topologyHash", hash},
           {"topology", t},
           {"artifacts", resolved["artifacts"]},
           {"resources", Json::array()},
           {"captureCoverage",
            t["capture"]["required"] == true ? "pending" : "unavailable-development-mode"},
           {"capturePolicy", policy},
           {"captureEpoch", "0"},
           {"controllerGeneration", "1"},
           {"captures", Json::array()},
           {"captureHistory", Json::array()},
           {"createdAt", console::timestamp()}};
  } else {
    id = string(p, "runId");
    if (!state_["runs"].contains(id))
      throw Failure("not_found", 404);
    run = state_["runs"][id];
    if (string(p, "expectedRevision") != run["revision"].get<std::string>())
      throw Failure("stale_revision", 409);
    operation = string(p, "operation");
    if (operation != "stop" && operation != "resume" && operation != "destroy" &&
        operation != "recover")
      throw Failure("unsupported_operation");
    if (operation == "stop" && run["state"] != "ready" && run["state"] != "stopped")
      throw Failure("invalid_run_state", 409);
    if (operation == "resume" && run["state"] != "stopped")
      throw Failure("invalid_run_state", 409);
    run["revision"] = std::to_string(std::stoull(run["revision"].get<std::string>()) + 1);
  }
  auto jobid = uuid();
  Json job = {{"id", jobid},
              {"runId", id},
              {"operation", operation},
              {"state", "queued"},
              {"revision", run["revision"]},
              {"cancelRequested", false},
              {"createdAt", console::timestamp()},
              {"error", nullptr}};
  state_["runs"][id] = run;
  state_["jobs"][jobid] = job;
  state_["keys"][key] = {{"hash", fingerprint}, {"jobId", jobid}};
  save();
  condition_.notify_all();
  return {{"jobId", jobid}, {"runId", id}, {"revision", run["revision"]}};
}
void Engine::work() {
  for (;;) {
    std::string id;
    {
      std::unique_lock guard(mutex_);
      condition_.wait_for(guard, std::chrono::seconds(1), [&] {
        if (stopping_)
          return true;
        for (const auto &[key, j] : state_["jobs"].items())
          if (j["state"] == "queued")
            return true;
        return false;
      });
      if (stopping_)
        return;
      for (const auto &[key, j] : state_["jobs"].items())
        if (j["state"] == "queued") {
          id = key;
          break;
        }
      if (id.empty()) {
        guard.unlock();
        monitor();
        continue;
      }
      state_["jobs"][id]["state"] = "running";
      save();
    }
    try {
      execute(id);
    } catch (const std::exception &e) {
      Json failed_run;
      {
        std::lock_guard guard(mutex_);
        failed_run = state_["runs"][state_["jobs"][id]["runId"].get<std::string>()];
      }
      try {
        backend_.gate(failed_run, "quiesce");
      } catch (...) { /* Recovery retains all uncertain resources. */
      }
      std::lock_guard guard(mutex_);
      auto &job = state_["jobs"][id];
      job["state"] = "failed";
      job["error"] = e.what();
      job["finishedAt"] = console::timestamp();
      state_["runs"][job["runId"].get<std::string>()]["state"] = "reconciling";
      auto &failed = state_["runs"][job["runId"].get<std::string>()];
      if (failed["topology"]["capture"]["required"] == true) {
        failed["captureCoverage"] = "incomplete";
        failed["quiescenceRequestedAt"] = console::timestamp();
      }
      save();
    }
  }
}
void Engine::monitor() {
  try {
    m5_monitor();
  } catch (const std::exception &e) {
    std::lock_guard guard(mutex_);
    for (auto &[id, r] : state_["runs"].items())
      if (r["state"] != "destroyed")
        r["telemetryError"] = e.what();
    save();
  }
  Json run;
  {
    std::lock_guard guard(mutex_);
    for (const auto &[id, r] : state_["runs"].items())
      if (r["state"] == "ready" &&
          (r["topology"]["capture"]["required"] == true ||
           std::any_of(r["resources"].begin(), r["resources"].end(),
                       [](const Json &n) { return n["kind"] == "qemu"; }))) {
        run = r;
        break;
      }
  }
  if (run.is_null())
    return;
  auto id = run["id"].get<std::string>();
  try {
    auto status = run["topology"]["capture"]["required"] == true
                      ? backend_.capture_control(run, "status")
                      : Json::array();
    backend_.gate(run, "renew");
    std::lock_guard guard(mutex_);
    state_["runs"][id]["captureObservations"] = status;
    state_["runs"][id]["lastHealthyAt"] = console::timestamp();
    save();
  } catch (const std::exception &e) {
    auto observed = console::timestamp();
    bool held = false;
    try {
      backend_.gate(run, "quiesce");
      held = true;
    } catch (...) {
    }
    std::lock_guard guard(mutex_);
    auto &r = state_["runs"][id];
    r["state"] = "reconciling";
    r["captureCoverage"] = "incomplete";
    r["captureError"] = e.what();
    r["failureObservedAt"] = observed;
    r["quiescenceRequestedAt"] = observed;
    r["quiescenceAcknowledgedAt"] = held ? Json(console::timestamp()) : Json(nullptr);
    save();
  }
}
void Engine::close_captures(const std::string &id) {
  Json run;
  {
    std::lock_guard guard(mutex_);
    run = state_["runs"][id];
  }
  if (run["topology"]["capture"]["required"] == true) {
    auto observations = backend_.capture_control(run, "stop");
    std::lock_guard guard(mutex_);
    state_["runs"][id]["captureObservations"] = observations;
    auto coverage = run.value("captureCoverage", "");
    bool incomplete = coverage == "incomplete" || coverage == "closed-incomplete";
    for (const auto &s : observations)
      incomplete = incomplete || s.value("state", "") != "closed";
    state_["runs"][id]["captureCoverage"] = incomplete ? "closed-incomplete" : "closed";
    save();
  }
}
void Engine::cleanup(const std::string &id) {
  m5_clear(id);
  close_captures(id);
  Json run;
  {
    std::lock_guard guard(mutex_);
    run = state_["runs"][id];
  }
  for (const auto &s : run.value("sessions", Json::array()))
    terminal::stop(s, run["controllerGeneration"]);
  // Every resource has a durable intent even if its creation never returned.
  for (std::size_t index = run["resources"].size(); index > 0; --index) {
    auto resource = run["resources"][index - 1];
    if (resource.value("state", "") == "removed")
      continue;
    backend_.remove(run, resource);
    {
      std::lock_guard guard(mutex_);
      state_["runs"][id]["resources"][index - 1]["state"] = "removed";
      save();
    }
  }
}
void Engine::execute(const std::string &jobid) {
  Json run, job;
  auto snapshot = [&] {
    std::lock_guard guard(mutex_);
    job = state_["jobs"][jobid];
    run = state_["runs"][job["runId"].get<std::string>()];
  };
  snapshot();
  auto id = run["id"].get<std::string>();
  auto operation = job["operation"].get<std::string>();
  if (operation == "fault.apply" || operation == "fault.remove") {
    m5_execute(jobid);
    return;
  }
  auto cancelled = [&] {
    std::lock_guard guard(mutex_);
    if (stopping_)
      throw Failure("agent_stopping");
    return state_["jobs"][jobid]["cancelRequested"] == true;
  };
  auto state = [&](const std::string &value) {
    std::lock_guard guard(mutex_);
    state_["runs"][id]["state"] = value;
    save();
  };
  bool recording = run["topology"]["capture"]["required"] == true;
  auto arm = [&] {
    if (!recording)
      return;
    snapshot();
    if (!run.value("captures", Json::array()).empty())
      backend_.capture_control(run, "stop");
    {
      std::lock_guard guard(mutex_);
      auto &r = state_["runs"][id];
      for (const auto &c : r["captures"])
        r["captureHistory"].push_back(c);
      r["captures"] = Json::array();
      r["captureEpoch"] = std::to_string(std::stoull(r["captureEpoch"].get<std::string>()) + 1);
      save();
    }
    snapshot();
    auto planned = backend_.capture_plan(run);
    {
      std::lock_guard guard(mutex_);
      state_["runs"][id]["captures"] = planned; // durable launch intents before systemd
      save();
    }
    snapshot();
    auto observed = backend_.capture_control(run, "arm");
    {
      std::lock_guard guard(mutex_);
      auto &r = state_["runs"][id];
      r["captureObservations"] = observed;
      r["captureCoverage"] = "armed";
      r["armedAt"] = console::timestamp();
      r["state"] = "armed";
      save();
    }
    snapshot();
  };
  if (operation == "start") {
    if (recording)
      (void)backend_.capture_plan(run); // reserve capacity before resource effects
    backend_.preflight(run["topology"], run["artifacts"]);
    bool cancel = cancelled();
    for (auto resource : resources(run)) {
      if (cancel)
        break;
      resource["state"] = "intent";
      {
        std::lock_guard guard(mutex_);
        state_["runs"][id]["resources"].push_back(resource);
        save();
      }
      snapshot();
      if (resource["kind"] == "qemu")
        detail::checkpoint("qemu.intent");
      auto identity = backend_.prepare(run, resource);
      if (resource["kind"] == "qemu")
        detail::checkpoint("qemu.readback");
      {
        std::lock_guard guard(mutex_);
        auto &saved = state_["runs"][id]["resources"].back();
        saved["identity"] = identity;
        saved["observedAt"] = console::timestamp();
        saved["state"] = "prepared";
        save();
      }
      if (resource["kind"] == "qemu")
        detail::checkpoint("qemu.completion");
      cancel = cancelled();
    }
    if (!cancel) {
      snapshot();
      arm();
      state("converging");
      backend_.activate(run);
      cancel = cancelled();
    }
    if (!cancel) {
      snapshot();
      if (recording)
        backend_.capture_control(run, "status");
      backend_.gate(run, "release");
      cancel = cancelled();
    }
    if (cancel) {
      state("compensating");
      snapshot();
      try {
        backend_.gate(run, "quiesce");
      } catch (...) {
      }
      cleanup(id);
      state("destroyed");
      std::lock_guard guard(mutex_);
      state_["jobs"][jobid]["state"] = "cancelled";
      state_["jobs"][jobid]["finishedAt"] = console::timestamp();
      save();
      return;
    }
    state("ready");
  } else if (operation == "stop") {
    backend_.gate(run, "quiesce");
    if (recording)
      close_captures(id);
    state("stopped");
  } else if (operation == "resume") {
    m5_monitor();
    snapshot();
    if (run["state"] != "stopped")
      throw Failure("fault_reconciliation_required");
    arm();
    backend_.activate(run);
    if (recording)
      backend_.capture_control(run, "status");
    m5_monitor();
    snapshot();
    if (run["state"] == "reconciling")
      throw Failure("fault_reconciliation_required");
    backend_.gate(run, "release");
    state("ready");
  } else {
    state("destroying");
    try {
      backend_.gate(run, "quiesce");
    } catch (...) {
    }
    cleanup(id);
    state("destroyed");
  }
  snapshot();
  Json observation = backend_.observe(run);
  std::lock_guard guard(mutex_);
  state_["runs"][id]["observation"] = observation;
  if (recording) {
    state_["runs"][id]["captureCoverage"] =
        run["state"] == "ready"                                   ? "recording"
        : run.value("captureCoverage", "") == "closed-incomplete" ? "closed-incomplete"
                                                                  : "closed";
    if (run["state"] == "ready")
      state_["runs"][id]["releasedAt"] = console::timestamp();
  }
  state_["jobs"][jobid]["state"] = "succeeded";
  state_["jobs"][jobid]["finishedAt"] = console::timestamp();
  save();
}
Json Engine::inventory(Json logical) {
  std::lock_guard guard(mutex_);
  for (const auto &[id, run] : state_["runs"].items()) {
    if (run["topologyHash"] != logical["topologyHash"] || run["state"] == "destroyed")
      continue;
    logical["run"] = {{"id", id},
                      {"state", run["state"]},
                      {"revision", run["revision"]},
                      {"captureCoverage", run["captureCoverage"]}};
    logical["runtimeFreshness"] = "snapshot";
    logical["runtimeObservedAt"] =
        run.contains("observation") ? run["observation"]["observedAt"] : Json(nullptr);
    for (auto type : {"nodes", "edges"})
      for (auto &item : logical[type]) {
        auto key = std::string(type == std::string("nodes") ? "node/" : "edge/") +
                   item["id"].get<std::string>();
        for (const auto &resource : run["resources"])
          if (resource["key"] == key && resource.value("state", "") == "prepared" &&
              resource.contains("identity")) {
            item["runtime"] = {
                {"state", run["state"] == "reconciling" ? "uncertain" : "observed"},
                {"observedAt", resource["observedAt"]},
                {"mappingEpoch", "1"},
                {"identity", resource["identity"]},
                {"reason", "Agent-owned identity snapshot; no continuous runtime monitor"}};
          }
        if (type == std::string("edges"))
          item["captureState"] = run["captureCoverage"];
      }
    break;
  }
  return logical;
}
} // namespace graphlab::runtime
