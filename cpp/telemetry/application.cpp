#include <graphlab/application_telemetry.hpp>
#include <graphlab/telemetry.hpp>
#include <lab_support/application_telemetry.hpp>
#include <sqlite3.h>
namespace graphlab::application_telemetry {
using runtime::Json;
namespace {
struct Statement {
  sqlite3_stmt *s = nullptr;
  Statement(sqlite3 *db, const char *sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
      throw runtime::Failure("application_telemetry_database");
  }
  ~Statement() { sqlite3_finalize(s); }
  void str(int i, const std::string &v) {
    sqlite3_bind_text(s, i, v.data(), v.size(), SQLITE_TRANSIENT);
  }
  void number(int i, sqlite3_int64 n) { sqlite3_bind_int64(s, i, n); }
  bool row() {
    auto n = sqlite3_step(s);
    if (n != SQLITE_ROW && n != SQLITE_DONE)
      throw runtime::Failure("application_telemetry_database");
    return n == SQLITE_ROW;
  }
  Json body() { return Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(s, 0))); }
};
void execute(sqlite3 *db, const char *sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw runtime::Failure("application_telemetry_database");
}
} // namespace
void initialize(sqlite3 *db) {
  execute(db, "CREATE TABLE IF NOT EXISTS application_latest(run TEXT,node TEXT,time INTEGER,body "
              "TEXT,PRIMARY KEY(run,node)); CREATE TABLE IF NOT EXISTS application_history(id "
              "INTEGER PRIMARY KEY,run TEXT,node TEXT,time INTEGER,body TEXT); CREATE INDEX IF NOT "
              "EXISTS application_history_scope ON application_history(run,node,id);");
}
static void ingest_series(sqlite3 *db, const std::string &run, const std::string &node,
                          const std::string &key, const std::string &instance, const Json &report,
                          const std::string &collector) {
  Json old = nullptr;
  {
    Statement q(db, "SELECT body FROM application_latest WHERE run=? AND node=?");
    q.str(1, run);
    q.str(2, key);
    if (q.row())
      old = q.body();
  }
  bool replaced =
      !old.is_null() && !instance.empty() && old.value("workloadInstance", "") != instance;
  if (replaced)
    old = nullptr;
  auto validated = lab_support::application_telemetry::validate(report);
  if (!old.is_null() && old["report"]["epoch"] == validated["epoch"] &&
      old["report"]["sequence"] == validated["sequence"]) {
    if (old["report"] != validated)
      throw runtime::Failure("application_report_sequence_conflict");
    return;
  }
  if (!old.is_null() && old["report"]["stream"] != validated["stream"])
    throw runtime::Failure("application_stream_changed");
  auto body = lab_support::application_telemetry::derive(old, validated, collector);
  body["node"] = node;
  if (!instance.empty())
    body["workloadInstance"] = instance;
  if (replaced)
    body["gapReason"] = "workload_instance_changed";
  if (report.contains("edge"))
    body["edge"] = report["edge"];
  body["runId"] = run;
  body["observedAt"] = console::timestamp();
  body["receivedMonotonicNs"] = std::to_string(telemetry::monotonic());
  body["source"] = "identity-checked-workload-status";
  if (body.dump().size() > 4096)
    throw runtime::Failure("application_record_size");
  execute(db, "BEGIN IMMEDIATE");
  try {
    Statement latest(db, "INSERT OR REPLACE INTO application_latest VALUES(?,?,?,?)");
    latest.str(1, run);
    latest.str(2, key);
    latest.number(3, telemetry::wall());
    latest.str(4, body.dump());
    latest.row();
    Statement history(db, "INSERT INTO application_history(run,node,time,body) VALUES(?,?,?,?)");
    history.str(1, run);
    history.str(2, key);
    history.number(3, telemetry::wall());
    history.str(4, body.dump());
    history.row();
    Statement prune(db, "DELETE FROM application_history WHERE time<?");
    prune.number(1, telemetry::wall() - 86400);
    prune.row();
    execute(db, "DELETE FROM application_history WHERE id IN (SELECT id FROM application_history "
                "ORDER BY id DESC LIMIT -1 OFFSET 10000); DELETE FROM application_latest WHERE "
                "rowid IN (SELECT rowid FROM application_latest ORDER BY time DESC,rowid DESC "
                "LIMIT -1 OFFSET 1000);");
    // Explicit encoded-body budgets are independent of SQLite file allocation.
    for (auto table : {"application_latest", "application_history"}) {
      auto budget = std::string(table) == "application_latest" ? 4 * 1024 * 1024 : 32 * 1024 * 1024;
      for (;;) {
        Statement total(
            db, ("SELECT coalesce(sum(length(CAST(body AS BLOB))),0) FROM " + std::string(table))
                    .c_str());
        total.row();
        if (sqlite3_column_int64(total.s, 0) <= budget)
          break;
        execute(db, ("DELETE FROM " + std::string(table) + " WHERE rowid IN (SELECT rowid FROM " +
                     table + " ORDER BY time,rowid LIMIT 100)")
                        .c_str());
      }
    }
    execute(db, "COMMIT");
  } catch (...) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
void ingest(sqlite3 *db, const std::string &run, const std::string &node, const Json &report,
            const std::string &collector) {
  if (report.at("apiVersion") != "graphlab.application-telemetry/v1")
    throw runtime::Failure("application_report_requires_edge_binding");
  ingest_series(db, run, node, node, "", report, collector);
}
void ingest_edge(sqlite3 *db, const std::string &run, const std::string &node,
                 const std::string &instance, const Json &topology, const Json &report,
                 const std::string &collector) {
  auto r = lab_support::application_telemetry::validate(report);
  if (r.at("apiVersion") != "graphlab.application-edge-telemetry/v1" || instance.empty())
    throw runtime::Failure("application_edge_identity");
  bool bound = false;
  for (const auto &edge :
       topology.value("application", Json::object()).value("edges", Json::array()))
    if (edge.at("id") == r.at("edge") && edge.at(r.at("endpoint").get<std::string>()) == node)
      bound = true;
  if (!bound)
    throw runtime::Failure("application_edge_identity");
  // Node/edge/endpoint define one stream. Changing its name is rejected, not added as a second
  // total.
  auto key =
      node + "/" + r.at("edge").get<std::string>() + "/" + r.at("endpoint").get<std::string>();
  ingest_series(db, run, node, key, instance, r, collector);
}
Json query(sqlite3 *db, const std::string &run, const Json &p, const std::string &collector) {
  for (const auto &[k, v] : p.items())
    if (k != "runId" && k != "node" && k != "limit" && k != "edge")
      throw runtime::Failure("unknown_application_query_field");
  if (p.contains("limit") &&
      (!p["limit"].is_number_integer() || p["limit"] < 1 || p["limit"] > 200))
    throw runtime::Failure("invalid_application_query_limit");
  auto edge = p.value("edge", std::string());
  if (edge.size() > 64)
    throw runtime::Failure("invalid_application_edge");
  auto node = p.value("node", std::string());
  if (node.size() > 128)
    throw runtime::Failure("invalid_application_node");
  Json current = Json::array(), history = Json::array();
  {
    Statement q(db, "SELECT body FROM application_latest WHERE run=? AND (?='' OR "
                    "json_extract(body,'$.node')=?) AND ((?='' AND json_extract(body,'$.edge') IS "
                    "NULL) OR json_extract(body,'$.edge')=?) ORDER BY "
                    "node LIMIT 1000");
    q.str(1, run);
    q.str(2, node);
    q.str(3, node);
    q.str(4, edge);
    q.str(5, edge);
    while (q.row()) {
      auto b = q.body();
      auto same = b["collectorEpoch"] == collector;
      auto ns = std::stoull(b["receivedMonotonicNs"].get<std::string>());
      b["stale"] = !same || telemetry::monotonic() - ns >= 15000000000ull;
      current.push_back(b);
    }
  }
  const int limit = p.value("limit", 100);
  {
    Statement q(db, "SELECT body FROM application_history WHERE run=? AND (?='' OR "
                    "json_extract(body,'$.node')=?) AND ((?='' AND json_extract(body,'$.edge') IS "
                    "NULL) OR json_extract(body,'$.edge')=?) AND "
                    "time>=? ORDER BY id DESC LIMIT ?");
    q.str(1, run);
    q.str(2, node);
    q.str(3, node);
    q.str(4, edge);
    q.str(5, edge);
    q.number(6, telemetry::wall() - 86400);
    q.number(7, limit + 1);
    while (q.row())
      history.push_back(q.body());
  }
  bool truncated = history.size() > static_cast<std::size_t>(limit);
  if (truncated)
    history.erase(history.end() - 1);
  return {{"apiVersion", "graphlab.application-telemetry/v1"},
          {"current", current},
          {"items", history},
          {"truncated", truncated},
          {"retentionSeconds", 86400},
          {"globalHistoryLimit", 10000},
          {"globalLatestLimit", 1000},
          {"globalHistoryBodyBytes", 33554432},
          {"globalLatestBodyBytes", 4194304},
          {"samplingIntervalSeconds", 5}};
}
} // namespace graphlab::application_telemetry
