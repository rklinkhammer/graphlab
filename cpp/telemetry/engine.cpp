#include <graphlab/application_telemetry.hpp>
#include <graphlab/telemetry.hpp>
#include <regex>
namespace graphlab::runtime {
namespace {
void fields(const Json &p, std::initializer_list<std::string_view> names) {
  if (!p.is_object())
    throw Failure("invalid_params");
  for (const auto &[k, v] : p.items())
    if (std::find(names.begin(), names.end(), k) == names.end())
      throw Failure("unknown_field");
}
void event(Json &r, const std::string &type, const Json &detail) {
  if (!r.contains("timeline"))
    r["timeline"] = Json::array();
  auto &list = r["timeline"];
  list.push_back({{"kind", type},
                  {"at", console::timestamp()},
                  {"monotonicNs", std::to_string(telemetry::monotonic())},
                  {"bootId", telemetry::boot()},
                  {"detail", detail}});
  if (list.size() > 4096)
    list.erase(list.begin());
}
bool live(const Json &f) { return f["state"] != "removed" && f["state"] != "cancelled"; }
void bump(Json &r) {
  r["revision"] = std::to_string(std::stoull(r["revision"].get<std::string>()) + 1);
}
} // namespace
std::optional<Json> Engine::m5_dispatch(const Json &request, uid_t principal) {
  auto method = request["method"].get<std::string>();
  if (method != "telemetry" && method != "timeline" && method != "faults" &&
      method != "fault.preview" && method != "fault.apply" && method != "fault.remove" &&
      method != "application-telemetry")
    return std::nullopt;
  const auto &p = request["params"];
  auto id = p.at("runId").get<std::string>();
  if (!state_["runs"].contains(id))
    throw Failure("not_found", 404);
  auto &r = state_["runs"][id];
  if (method == "application-telemetry") {
    auto result = application_telemetry::query(db_, id, p, collector_);
    result["errors"] = r.value("applicationTelemetryErrors", Json::object());
    result["observationProfile"] = r.value("observationProfile", "full");
    result["runState"] = r["state"];
    for (auto &value : result["current"])
      if (r["state"] == "destroyed" ||
          result["errors"].contains(value["node"].get<std::string>()) ||
          result["errors"].contains("collector"))
        value["stale"] = true;
    return result;
  }
  if (method == "telemetry") {
    auto result = telemetry::query(db_, id, p);
    auto current = samples_.value(id, Json::object());
    for (auto &[edge, s] : current.items())
      s["stale"] = telemetry::monotonic() - std::stoull(s["monotonicNs"].get<std::string>()) >=
                   3000000000ull;
    result["current"] = current;
    result["health"] = r.value("health", Json(nullptr));
    result["healthMonotonicNs"] = r.value("healthMonotonicNs", Json(nullptr));
    result["samplingIntervalSeconds"] = 1;
    result["observationProfile"] = r.value("observationProfile", "full");
    result["lastCollectionDurationNs"] = r.value("collectionDurationNs", Json(nullptr));
    result["collectorError"] = r.value("telemetryError", Json(nullptr));
    return result;
  }
  if (method == "timeline" || method == "faults") {
    fields(p, {"runId"});
    if (method == "faults")
      return Json{{"items", r.value("faults", Json::object())}, {"revision", r["revision"]}};
    auto events = r.value("timeline", Json::array());
    for (const auto &[jid, j] : state_["jobs"].items())
      if (j["runId"] == id)
        events.push_back(
            {{"kind", "job"}, {"at", j.value("finishedAt", j["createdAt"])}, {"detail", j}});
    for (const auto &c : r.value("captures", Json::array()))
      events.push_back(
          {{"kind", "capture"},
           {"at", r.value("armedAt", r["createdAt"])},
           {"detail", {{"id", c["id"]}, {"edge", c["edge"]}, {"coverage", r["captureCoverage"]}}}});
    for (const auto &s : r.value("sessions", Json::array()))
      events.push_back(
          {{"kind", "console"},
           {"at", s["createdAt"]},
           {"detail", {{"id", s["id"]}, {"node", s["node"]}, {"recordInput", s["recordInput"]}}}});
    std::sort(events.begin(), events.end(),
              [](const Json &a, const Json &b) { return a["at"] < b["at"]; });
    return Json{{"items", events},
                {"clockBasis", "UTC correlation; use boot/monotonic fields for intervals"}};
  }
  if (method == "fault.preview") {
    fields(p, {"runId", "fault"});
    auto f = telemetry::validate_fault(p.at("fault"));
    if (r["state"] != "ready" && r["state"] != "stopped")
      throw Failure("fault_run_not_available", 409);
    f["id"] = "preview";
    return backend_.fault(r, f, "plan");
  }
  fields(p, method == "fault.apply"
                ? std::initializer_list<std::string_view>{"runId", "fault", "idempotencyKey",
                                                          "expectedRevision"}
                : std::initializer_list<std::string_view>{"runId", "faultId", "idempotencyKey",
                                                          "expectedRevision"});
  auto key = p.at("idempotencyKey").get<std::string>();
  if (!std::regex_match(key, std::regex("[A-Za-z0-9_-]{8,80}")))
    throw Failure("invalid_idempotency_key");
  key = std::to_string(principal) + ":" + key;
  auto hash = lab_support::digest(request);
  if (state_["keys"].contains(key)) {
    auto old = state_["keys"][key];
    if (old["hash"] != hash)
      throw Failure("idempotency_conflict", 409);
    auto j = state_["jobs"][old["jobId"].get<std::string>()];
    return Json{
        {"jobId", j["id"]}, {"runId", id}, {"faultId", j["faultId"]}, {"revision", j["revision"]}};
  }
  if (p.at("expectedRevision") != r["revision"])
    throw Failure("stale_revision", 409);
  for (const auto &[k, j] : state_["jobs"].items())
    if (j["state"] == "queued" || j["state"] == "running")
      throw Failure("operation_in_progress", 409);
  if (state_["jobs"].size() >= 256)
    throw Failure("journal_job_capacity", 429);
  if (!r.contains("faults"))
    r["faults"] = Json::object();
  std::string fid;
  if (method == "fault.apply") {
    if (r["state"] != "ready" && r["state"] != "stopped")
      throw Failure("fault_run_not_available", 409);
    if (r["faults"].size() >= 256)
      throw Failure("fault_capacity", 429);
    auto f = telemetry::validate_fault(p.at("fault"));
    for (const auto &[k, old] : r["faults"].items())
      if (live(old) && old["edge"] == f["edge"] && old["direction"] == f["direction"])
        throw Failure("fault_direction_busy", 409);
    fid = console::random_hex(16);
    f["id"] = fid;
    f["placement"] = backend_.fault(r, f, "plan");
    f["state"] = "queued";
    f["bootId"] = telemetry::boot();
    r["faults"][fid] = f;
  } else {
    fid = p.at("faultId");
    if (!r["faults"].contains(fid))
      throw Failure("fault_not_found", 404);
  }
  bump(r);
  auto jid = console::random_hex(16); // jobs use UUID-shaped IDs for the existing HTTP job route
  jid = jid.substr(0, 8) + "-" + jid.substr(8, 4) + "-" + jid.substr(12, 4) + "-" +
        jid.substr(16, 4) + "-" + jid.substr(20);
  state_["jobs"][jid] = {{"id", jid},
                         {"runId", id},
                         {"operation", method},
                         {"faultId", fid},
                         {"state", "queued"},
                         {"revision", r["revision"]},
                         {"cancelRequested", false},
                         {"createdAt", console::timestamp()},
                         {"error", nullptr}};
  state_["keys"][key] = {{"hash", hash}, {"jobId", jid}};
  event(r, "fault-admitted", {{"faultId", fid}, {"jobId", jid}, {"operation", method}});
  save();
  condition_.notify_all();
  return Json{{"jobId", jid}, {"runId", id}, {"faultId", fid}, {"revision", r["revision"]}};
}
void Engine::m5_execute(const std::string &jid) {
  Json run, f;
  std::string id, fid, op;
  {
    std::lock_guard lock(mutex_);
    auto &j = state_["jobs"][jid];
    id = j["runId"];
    fid = j["faultId"];
    op = j["operation"];
    auto &r = state_["runs"][id];
    auto &fault = r["faults"][fid];
    if (op == "fault.apply" && j["cancelRequested"] == true) {
      fault["state"] = "cancelled";
      j["state"] = "cancelled";
      j["finishedAt"] = console::timestamp();
      event(r, "fault-cancelled", {{"faultId", fid}});
      save();
      return;
    }
    if (op == "fault.remove" && !live(fault)) {
      j["state"] = "succeeded";
      j["finishedAt"] = console::timestamp();
      save();
      return;
    }
    fault["state"] = op == "fault.apply" ? "applying" : "removing";
    save();
    run = r;
    f = fault;
  }
  detail::checkpoint(op + ".intent");
  auto observed = backend_.fault(run, f, op == "fault.apply" ? "apply" : "remove");
  detail::checkpoint(op + ".readback");
  bool cancel = false;
  {
    std::lock_guard lock(mutex_);
    cancel = op == "fault.apply" && state_["jobs"][jid]["cancelRequested"] == true;
  }
  if (cancel)
    backend_.fault(run, f, "remove");
  std::lock_guard lock(mutex_);
  auto &r = state_["runs"][id];
  auto &saved = r["faults"][fid];
  auto &j = state_["jobs"][jid];
  saved["state"] = cancel ? "cancelled" : op == "fault.apply" ? "active" : "removed";
  saved["observed"] = observed;
  if (op == "fault.apply" && !cancel) {
    saved["activatedAt"] = console::timestamp();
    saved["activatedMonotonicNs"] = std::to_string(telemetry::monotonic());
    saved["expiresMonotonicNs"] = std::to_string(
        telemetry::monotonic() + f["durationSeconds"].get<std::uint64_t>() * 1000000000ull);
  }
  j["state"] = cancel ? "cancelled" : "succeeded";
  j["finishedAt"] = console::timestamp();
  event(r, "fault-" + saved["state"].get<std::string>(),
        {{"faultId", fid}, {"placement", saved["placement"]}});
  detail::checkpoint(op + ".completion");
  save();
}
void Engine::m5_clear(const std::string &id) {
  Json run;
  {
    std::lock_guard lock(mutex_);
    run = state_["runs"][id];
  }
  for (const auto &[fid, f] : run["faults"].items())
    if (live(f)) {
      backend_.fault(run, f, "remove");
      std::lock_guard lock(mutex_);
      auto &r = state_["runs"][id];
      r["faults"][fid]["state"] = "removed";
      event(r, "fault-removed", {{"faultId", fid}, {"reason", "run-cleanup"}});
      save();
    }
}
void Engine::m5_recover() {
  for (auto &[id, r] : state_["runs"].items())
    if (r["state"] != "destroyed")
      for (auto &[fid, f] : r["faults"].items())
        if (live(f)) {
          bool expired = f.value("bootId", "") != telemetry::boot() ||
                         f.value("state", "") != "active" ||
                         std::stoull(f.value("expiresMonotonicNs", "0")) <= telemetry::monotonic();
          if (expired) {
            try {
              backend_.fault(r, f, "remove");
              r["faults"][fid]["state"] = "removed";
              event(r, "fault-recovered", {{"faultId", fid}});
            } catch (const std::exception &e) {
              m5_fault_failure(id, fid, e.what());
            }
          }
        }
  save();
}
void Engine::m5_fault_failure(const std::string &id, const std::string &fid,
                              const std::string &error) {
  Json run;
  {
    std::lock_guard lock(mutex_);
    auto &r = state_["runs"][id];
    r["state"] = "reconciling";
    r["faults"][fid]["state"] = "recovery-required";
    r["faultRecoveryError"] = error;
    r["quiescenceRequestedAt"] = console::timestamp();
    r["quiescenceAcknowledgedAt"] = nullptr;
    r.erase("quiescenceError");
    bump(r);
    event(r, "fault-recovery-required", {{"faultId", fid}, {"error", error}});
    save();
    run = r;
  }
  std::string failure;
  try {
    backend_.gate(run, "quiesce");
  } catch (const std::exception &e) {
    failure = e.what();
  }
  std::lock_guard lock(mutex_);
  auto &r = state_["runs"][id];
  if (failure.empty())
    r["quiescenceAcknowledgedAt"] = console::timestamp();
  else
    r["quiescenceError"] = failure;
  event(r, "fault-quiescence",
        {{"faultId", fid},
         {"acknowledged", failure.empty()},
         {"error", failure.empty() ? Json(nullptr) : Json(failure)}});
  save();
}
void Engine::m5_monitor() {
  Json runs;
  {
    std::lock_guard lock(mutex_);
    runs = state_["runs"];
  }
  for (auto &[id, run] : runs.items()) {
    if (run["state"] == "destroyed")
      continue;
    for (auto &[fid, f] : run["faults"].items())
      if (f["state"] == "active" &&
          std::stoull(f["expiresMonotonicNs"].get<std::string>()) <= telemetry::monotonic()) {
        {
          std::lock_guard lock(mutex_);
          state_["runs"][id]["faults"][fid]["state"] = "removing";
          save();
        }
        try {
          backend_.fault(run, f, "remove");
          std::lock_guard lock(mutex_);
          auto &r = state_["runs"][id];
          r["faults"][fid]["state"] = "removed";
          bump(r);
          event(r, "fault-expired", {{"faultId", fid}});
          save();
        } catch (const std::exception &e) {
          m5_fault_failure(id, fid, e.what());
          run["state"] = "reconciling";
        }
      }
    if ((run["state"] != "ready" && run["state"] != "stopped") ||
        run.value("observationProfile", "full") == "minimal")
      continue;
    auto began = telemetry::monotonic();
    Json values;
    if (began - std::stoull(run.value("healthMonotonicNs", "0")) >= 5000000000ull) {
      Json health;
      try {
        health = backend_.observe(run);
      } catch (const std::exception &e) {
        health = {{"error", e.what()}};
      }
      std::lock_guard lock(mutex_);
      auto &r = state_["runs"][id];
      r["applicationTelemetryErrors"] = Json::object();
      if (health.contains("error"))
        r["applicationTelemetryErrors"]["collector"] = health["error"];
      for (const auto &node : health.value("nodes", Json::array())) {
        auto name = node.value("id", std::string());
        bool owned = false;
        for (const auto &resource : run["resources"])
          if (resource["kind"] == "container" && resource["logical"] == name)
            owned = true;
        if (!owned)
          continue;
        if (node.contains("error")) {
          r["applicationTelemetryErrors"][name] = node["error"];
          continue;
        }
        const auto gate = node.value("gate", Json::object());
        if (!gate.contains("applicationTelemetry"))
          continue;
        try {
          application_telemetry::ingest(db_, id, name, gate["applicationTelemetry"], collector_);
        } catch (const std::exception &) {
          r["applicationTelemetryErrors"][name] = "application_report_rejected";
        }
      }
      r["health"] = health;
      r["healthMonotonicNs"] = std::to_string(telemetry::monotonic());
      event(r, "health", health);
    }
    try {
      values = backend_.telemetry(run);
    } catch (const std::exception &e) {
      values = Json::array();
      for (const auto &r : run["resources"])
        if (r["kind"] == "edge")
          values.push_back({{"edge", r["logical"]}, {"valid", false}, {"reason", e.what()}});
    }
    std::lock_guard lock(mutex_);
    auto &r = state_["runs"][id];
    r["collectionDurationNs"] = std::to_string(telemetry::monotonic() - began);
    r.erase("telemetryError");
    for (auto &s : values) {
      s["runId"] = id;
      s["generation"] = run["controllerGeneration"];
      s["collectorEpoch"] = collector_;
      s["bootId"] = telemetry::boot();
      s["monotonicNs"] = s.value("monotonicNs", std::to_string(telemetry::monotonic()));
      s["at"] = console::timestamp();
      auto edge = s["edge"].get<std::string>();
      if (!samples_[id].is_object())
        samples_[id] = Json::object();
      auto old = samples_[id].value(edge, Json(nullptr));
      auto derived = lab_support::telemetry::derive(old, s);
      samples_[id][edge] = derived;
      telemetry::append(db_, derived);
      if (!derived["gapReason"].is_null())
        event(r, "telemetry-gap", {{"edge", edge}, {"reason", derived["gapReason"]}});
    }
    save();
  }
}
} // namespace graphlab::runtime
