#include <graphlab/runtime.hpp>
#include <regex>
namespace graphlab::runtime {
namespace {
Json resource(const Json &run, const std::string &node) {
  for (const auto &r : run["resources"])
    if (r["logical"] == node && r["kind"] == "container" && r.contains("identity") &&
        r.value("state", "") != "removed")
      return r;
  throw Failure("source_node_unavailable", 404);
}
void validate(const Json &p, bool command) {
  const std::vector<std::string> allowed =
      command ? std::vector<std::string>{"runId", "node",   "apiVersion", "instance",
                                         "epoch", "source", "action",     "requestId"}
              : std::vector<std::string>{"runId", "node"};
  if (!p.is_object() || p.size() != allowed.size() || p.dump().size() > 1024)
    throw Failure("source_request_invalid");
  for (const auto &k : allowed)
    if (!p.contains(k) || !p[k].is_string() || p[k].get<std::string>().empty() ||
        p[k].get<std::string>().size() > 128)
      throw Failure("source_request_invalid");
  if (command &&
      (p["apiVersion"] != "graphlab.source-control/v1" ||
       (p["action"] != "pause" && p["action"] != "resume") ||
       !std::regex_match(p["requestId"].get<std::string>(), std::regex("[A-Za-z0-9_-]{1,64}"))))
    throw Failure("source_request_invalid");
}
void available(const Json &run) {
  if (run["state"] != "ready" && run["state"] != "stopped")
    throw Failure("source_run_unavailable", 409);
}
Json wire(const Json &p) {
  return {{"apiVersion", p["apiVersion"]},
          {"epoch", p["epoch"]},
          {"source", p["source"]},
          {"action", p["action"]},
          {"requestId", p["requestId"]}};
}
void valid_capability(const Json &status) {
  if (status.value("apiVersion", "") != "graphlab.source-control/v1" ||
      status.dump().size() > 2048 || !status.contains("sources") || !status["sources"].is_array() ||
      status["sources"].empty() || status["sources"].size() > 4 ||
      !std::regex_match(status.value("epoch", ""), std::regex("[a-f0-9]{32}")))
    throw Failure("source_capability_invalid");
  std::vector<std::string> seen;
  for (const auto &s : status["sources"]) {
    auto id = s.value("id", "");
    if (!std::regex_match(id, std::regex("[A-Za-z][A-Za-z0-9_-]{0,63}")) ||
        (s.value("state", "") != "paused" && s.value("state", "") != "running") ||
        !std::regex_match(s.value("generatedDatagrams", ""), std::regex("0|[1-9][0-9]{0,19}")) ||
        std::find(seen.begin(), seen.end(), id) != seen.end())
      throw Failure("source_capability_invalid");
    seen.push_back(id);
  }
}
void capability(const Json &status, const Json &p) {
  valid_capability(status);
  if (status.value("epoch", "") != p.value("epoch", ""))
    throw Failure("source_epoch_changed", 409);
  for (const auto &s : status["sources"])
    if (s.value("id", "") == p["source"].get<std::string>())
      return;
  throw Failure("source_control_unsupported");
}
} // namespace
std::optional<Json> Engine::source_dispatch(const Json &request, uid_t principal) {
  const auto method = request.value("method", "");
  if (method != "source-controls.query" && method != "source-controls.command")
    return {};
  auto p = request["params"];
  bool command = method == "source-controls.command";
  validate(p, command);
  auto runid = p["runId"].get<std::string>();
  if (!state_["runs"].contains(runid))
    throw Failure("not_found", 404);
  auto &run = state_["runs"][runid];
  if (!command) {
    Json records = Json::array();
    for (const auto &[key, c] : state_["sourceCommands"].items())
      if (c["request"]["runId"] == runid && c["request"]["node"] == p["node"])
        records.push_back(c);
    Json out = {{"commands", records}, {"runState", run["state"]}, {"capability", nullptr}};
    try {
      available(run);
      const auto r = resource(run, p["node"]);
      auto status = backend_.source_control(run, r, nullptr);
      valid_capability(status);
      out["capability"] = status;
      out["instance"] = r["identity"]["id"];
    } catch (const std::exception &e) {
      out["error"] = e.what();
    }
    return out;
  }
  // Global request-ID binding prevents an old ID selecting a replacement instance/source.
  auto key = lab_support::digest(Json::array({std::to_string(principal), p["requestId"]}));
  if (state_["sourceCommands"].contains(key)) {
    auto old = state_["sourceCommands"][key];
    if (old["request"] != p)
      throw Failure("source_request_conflict", 409);
    return old;
  }
  available(run);
  const auto r = resource(run, p["node"]);
  if (r["identity"]["id"] != p["instance"])
    throw Failure("source_instance_changed", 409);
  bool declared = false;
  if (run["topology"].contains("application"))
    for (const auto &edge : run["topology"]["application"]["edges"])
      if (edge["id"] == p["source"] && edge["source"] == p["node"])
        declared = true;
  if (!declared)
    throw Failure("source_not_declared");
  for (const auto &[id, j] : state_["jobs"].items())
    if (j["state"] == "queued" || j["state"] == "running")
      throw Failure("operation_in_progress", 409);
  std::size_t pending = 0;
  for (const auto &[id, c] : state_["sourceCommands"].items())
    if (c["outcome"] == "requested") {
      ++pending;
      if (c["request"]["runId"] == p["runId"] && c["request"]["node"] == p["node"] &&
          c["request"]["source"] == p["source"])
        throw Failure("source_command_pending", 409);
    }
  if (pending >= 8 || state_["sourceCommands"].size() >= 256)
    throw Failure("source_command_capacity", 429);
  capability(backend_.source_control(run, r, nullptr), p);
  Json c = {{"request", p},
            {"outcome", "requested"},
            {"requestedAt", console::timestamp()},
            {"deadlineMs",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 (std::chrono::steady_clock::now() + std::chrono::seconds(10)).time_since_epoch())
                 .count()}};
  state_["sourceCommands"][key] = c;
  save();
  condition_.notify_all();
  return c;
}
void Engine::source_work() {
  Json c, run, r;
  std::string key;
  {
    std::lock_guard guard(mutex_);
    for (const auto &[id, item] : state_["sourceCommands"].items())
      if (item["outcome"] == "requested") {
        key = id;
        c = item;
        break;
      }
    if (key.empty())
      return;
    run = state_["runs"][c["request"]["runId"].get<std::string>()];
  }
  std::string outcome = "acknowledged", error;
  try {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() >= c["deadlineMs"].get<std::int64_t>()) {
      outcome = "timed-out";
      throw Failure("source_queue_deadline");
    }
    available(run);
    r = resource(run, c["request"]["node"]);
    if (r["identity"]["id"] != c["request"]["instance"])
      throw Failure("source_instance_changed");
    capability(backend_.source_control(run, r, nullptr), c["request"]);
    auto command = wire(c["request"]);
    auto ack = backend_.source_control(run, r, command);
    command["outcome"] = "acknowledged";
    if (ack != command)
      throw Failure("source_acknowledgement_invalid");
  } catch (const std::exception &e) {
    error = e.what();
    if (outcome != "timed-out")
      outcome = error.find("timeout") != std::string::npos ? "timed-out" : "failed";
    if (error.size() > 256)
      error.resize(256);
  }
  std::lock_guard guard(mutex_);
  auto &record = state_["sourceCommands"][key];
  record["outcome"] = outcome;
  record["error"] = error;
  record["finishedAt"] = console::timestamp();
  save();
}
} // namespace graphlab::runtime
