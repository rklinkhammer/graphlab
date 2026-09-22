#include <fstream>
#include <graphlab/telemetry.hpp>
#include <sqlite3.h>
using graphlab::runtime::Json;
namespace graphlab::telemetry {
std::string boot() {
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  std::string b;
  f >> b;
  return b.empty() ? "non-linux" : b;
}
std::uint64_t monotonic() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::int64_t wall() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
namespace {
struct Statement {
  sqlite3_stmt *s = nullptr;
  Statement(sqlite3 *d, const char *q) {
    if (sqlite3_prepare_v2(d, q, -1, &s, nullptr) != SQLITE_OK)
      throw runtime::Failure("telemetry_database");
  }
  ~Statement() { sqlite3_finalize(s); }
  void str(int i, const std::string &v) {
    sqlite3_bind_text(s, i, v.data(), v.size(), SQLITE_TRANSIENT);
  }
  void n(int i, sqlite3_int64 v) { sqlite3_bind_int64(s, i, v); }
  void done() {
    if (sqlite3_step(s) != SQLITE_DONE)
      throw runtime::Failure("telemetry_write");
  }
};
} // namespace
void initialize(sqlite3 *d) {
  if (sqlite3_exec(d,
                   "CREATE TABLE IF NOT EXISTS telemetry(run TEXT, edge TEXT, resolution INTEGER, "
                   "time INTEGER, body TEXT, PRIMARY KEY(run,edge,resolution,time)); CREATE INDEX "
                   "IF NOT EXISTS telemetry_time ON telemetry(time);",
                   nullptr, nullptr, nullptr) != SQLITE_OK)
    throw runtime::Failure("telemetry_database");
}
void append(sqlite3 *d, const Json &s) {
  auto now = wall();
  for (int resolution : {1, 10, 60}) {
    auto bucket = now / resolution * resolution;
    Json aggregate = s;
    aggregate["sampleCount"] = 1;
    aggregate["validRateCount"] = s["forwardBitsPerSecond"].is_null() ? 0 : 1;
    aggregate["gapCount"] = s["gapReason"].is_null() ? 0 : 1;
    if (resolution > 1) {
      Statement q(d,
                  "SELECT body FROM telemetry WHERE run=? AND edge=? AND resolution=? AND time=?");
      q.str(1, s["runId"]);
      q.str(2, s["edge"]);
      q.n(3, resolution);
      q.n(4, bucket);
      if (sqlite3_step(q.s) == SQLITE_ROW) {
        auto old = Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(q.s, 0)));
        int valid = old["validRateCount"], add = aggregate["validRateCount"];
        aggregate["sampleCount"] = old["sampleCount"].get<int>() + 1;
        aggregate["validRateCount"] = valid + add;
        aggregate["gapCount"] = old["gapCount"].get<int>() + aggregate["gapCount"].get<int>();
        for (auto key : {"forwardBitsPerSecond", "reverseBitsPerSecond", "forwardPacketsPerSecond",
                         "reversePacketsPerSecond"})
          if (valid + add)
            aggregate[key] =
                ((valid ? old[key].get<double>() * valid : 0) + (add ? s[key].get<double>() : 0)) /
                (valid + add);
      }
    }
    aggregate["resolutionSeconds"] = resolution;
    aggregate["bucketUnixSeconds"] = std::to_string(bucket);
    Statement insert(d, "INSERT OR REPLACE INTO telemetry VALUES(?,?,?,?,?)");
    insert.str(1, s["runId"]);
    insert.str(2, s["edge"]);
    insert.n(3, resolution);
    insert.n(4, bucket);
    insert.str(5, aggregate.dump());
    insert.done();
  }
  Statement prune(d, "DELETE FROM telemetry WHERE (resolution=1 AND time<?) OR (resolution=10 AND "
                     "time<?) OR (resolution=60 AND time<?)");
  prune.n(1, now - 3600);
  prune.n(2, now - 86400);
  prune.n(3, now - 604800);
  prune.done();
  // A global storage ceiling complements time retention, including retained runs.
  Statement cap(d, "DELETE FROM telemetry WHERE rowid IN (SELECT rowid FROM telemetry ORDER BY "
                   "time DESC LIMIT -1 OFFSET 100000)");
  cap.done();
}
Json query(sqlite3 *d, const std::string &run, const Json &p) {
  for (const auto &[k, v] : p.items())
    if (k != "runId" && k != "edge" && k != "resolutionSeconds" && k != "fromUnixSeconds" &&
        k != "toUnixSeconds" && k != "limit" && k != "windowSeconds")
      throw runtime::Failure("unknown_telemetry_field");
  int resolution = p.value("resolutionSeconds", 1), limit = p.value("limit", 600);
  if ((resolution != 1 && resolution != 10 && resolution != 60) || limit < 1 || limit > 2000)
    throw runtime::Failure("invalid_telemetry_range");
  const auto now = wall();
  const int maximum = resolution == 1 ? 3600 : resolution == 10 ? 86400 : 604800;
  auto end = p.value("toUnixSeconds", std::to_string(now)),
       start = p.value("fromUnixSeconds", std::to_string(now - 300));
  if (p.contains("windowSeconds")) {
    const auto &window = p["windowSeconds"];
    if (p.contains("fromUnixSeconds") || p.contains("toUnixSeconds") ||
        !window.is_number_integer() || window < 1 || window > maximum)
      throw runtime::Failure("invalid_telemetry_range");
    start = std::to_string(now - window.get<int>());
  }
  auto number = [](const std::string &s) {
    if (s.empty() || s.size() > 12 || s.find_first_not_of("0123456789") != std::string::npos)
      throw runtime::Failure("invalid_telemetry_time");
    return std::stoll(s);
  };
  auto a = number(start), b = number(end);
  if (a > b || b - a > maximum)
    throw runtime::Failure("invalid_telemetry_range");
  auto edge = p.value("edge", std::string());
  Statement q(d, "SELECT body FROM telemetry WHERE run=? AND (?='' OR edge=?) AND resolution=? AND "
                 "time>=? AND time<=? ORDER BY time DESC,edge LIMIT ?");
  q.str(1, run);
  q.str(2, edge);
  q.str(3, edge);
  q.n(4, resolution);
  q.n(5, a);
  q.n(6, b);
  q.n(7, limit + 1);
  Json values = Json::array();
  while (sqlite3_step(q.s) == SQLITE_ROW)
    values.push_back(Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(q.s, 0))));
  bool truncated = values.size() > static_cast<std::size_t>(limit);
  if (truncated)
    values.erase(values.end() - 1);
  std::reverse(values.begin(), values.end());
  Statement extent(d, "SELECT MIN(time),MAX(time) FROM telemetry WHERE run=? AND "
                      "(?='' OR edge=?) AND resolution=?");
  extent.str(1, run);
  extent.str(2, edge);
  extent.str(3, edge);
  extent.n(4, resolution);
  Json oldest = nullptr, newest = nullptr;
  if (sqlite3_step(extent.s) == SQLITE_ROW && sqlite3_column_type(extent.s, 0) != SQLITE_NULL) {
    oldest = std::to_string(sqlite3_column_int64(extent.s, 0));
    newest = std::to_string(sqlite3_column_int64(extent.s, 1));
  }
  return {{"items", values},
          {"truncated", truncated},
          {"oldestAvailableUnixSeconds", oldest},
          {"newestAvailableUnixSeconds", newest},
          {"capacityRows", 100000},
          {"fromUnixSeconds", start},
          {"toUnixSeconds", end},
          {"serverUnixSeconds", std::to_string(wall())}};
}
Json validate_fault(const Json &f) {
  if (!f.is_object())
    throw runtime::Failure("invalid_fault");
  for (const auto &[k, v] : f.items())
    if (k != "edge" && k != "direction" && k != "kind" && k != "delayMs" && k != "lossPercent" &&
        k != "durationSeconds")
      throw runtime::Failure("unknown_fault_field");
  if (!f.contains("edge") || !f["edge"].is_string() || f["edge"].get<std::string>().size() > 80 ||
      f["edge"].get<std::string>().empty() || f.value("kind", "") != "netem" ||
      (f.value("direction", "") != "a-to-b" && f.value("direction", "") != "b-to-a"))
    throw runtime::Failure("invalid_fault");
  Json result = f;
  for (auto [key, maximum] :
       {std::pair{"delayMs", 5000}, {"lossPercent", 100}, {"durationSeconds", 3600}}) {
    auto v = f.value(key, Json(key == std::string("durationSeconds") ? 0 : 0));
    if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > maximum)
      throw runtime::Failure("invalid_fault_value");
    result[key] = v;
  }
  if (result["durationSeconds"] == 0 || (result["delayMs"] == 0 && result["lossPercent"] == 0))
    throw runtime::Failure("empty_fault");
  return result;
}
} // namespace graphlab::telemetry
