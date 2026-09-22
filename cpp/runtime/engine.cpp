#include <fcntl.h>
#include <graphlab/runtime.hpp>
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
                      {"kind", n["kind"] == "ovs-switch" ? "bridge" : "container"},
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
    : backend_(backend), catalog_(catalog) {
  struct stat info{};
  if (lstat(directory.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0077))
    throw Failure("state_directory_requires_0700");
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
    if (j["operation"] != "start")
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
                                                          "developmentMode"}
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
    if (!p.contains("developmentMode") || p["developmentMode"] != true)
      throw Failure("development_mode_required");
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
    if (t["capture"]["required"] != false)
      throw Failure("capture_barrier_requires_M3");
    for (const auto &[name, node] : t["nodes"].items()) {
      if (node["kind"] == "qemu")
        throw Failure("qemu_requires_M4");
      for (const auto &[port, v] : node["ports"].items())
        if (port.size() > 15)
          throw Failure("linux_interface_name_too_long");
    }
    id = uuid();
    operation = "start";
    run = {{"id", id},
           {"generation", "1"},
           {"revision", "1"},
           {"state", "preparing"},
           {"topologyHash", hash},
           {"topology", t},
           {"artifacts", resolved["artifacts"]},
           {"resources", Json::array()},
           {"captureCoverage", "unavailable-development-mode"},
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
      condition_.wait(guard, [&] {
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
      save();
    }
  }
}
void Engine::cleanup(const std::string &id) {
  Json run;
  {
    std::lock_guard guard(mutex_);
    run = state_["runs"][id];
  }
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
  if (operation == "start") {
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
      auto identity = backend_.prepare(run, resource);
      {
        std::lock_guard guard(mutex_);
        auto &saved = state_["runs"][id]["resources"].back();
        saved["identity"] = identity;
        saved["observedAt"] = console::timestamp();
        saved["state"] = "prepared";
        save();
      }
      cancel = cancelled();
    }
    if (!cancel) {
      snapshot();
      state("converging");
      backend_.activate(run);
      cancel = cancelled();
    }
    if (!cancel) {
      snapshot();
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
    state("stopped");
  } else if (operation == "resume") {
    backend_.activate(run);
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
          item["captureState"] = "unavailable-development-mode";
      }
    break;
  }
  return logical;
}
} // namespace graphlab::runtime
