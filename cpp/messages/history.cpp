#include <ctime>
#include <graphlab/message_history.hpp>
#include <regex>
#include <sqlite3.h>
#include <sys/stat.h>
namespace graphlab::messages {
using runtime::Failure;
namespace {
void sql(sqlite3 *d, const char *s) {
  if (sqlite3_exec(d, s, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw Failure(
        sqlite3_errcode(d) == SQLITE_FULL ? "message_storage_full" : "message_database_error", 503);
}
struct Q {
  sqlite3_stmt *s = nullptr;
  Q(sqlite3 *d, const char *q) {
    if (sqlite3_prepare_v2(d, q, -1, &s, nullptr) != SQLITE_OK)
      throw Failure("message_database_error", 503);
  }
  ~Q() { sqlite3_finalize(s); }
  void str(int n, const std::string &v) {
    sqlite3_bind_text(s, n, v.data(), v.size(), SQLITE_TRANSIENT);
  }
  void num(int n, std::int64_t v) { sqlite3_bind_int64(s, n, v); }
  bool next() {
    auto r = sqlite3_step(s);
    if (r != SQLITE_ROW && r != SQLITE_DONE)
      throw Failure(r == SQLITE_FULL ? "message_storage_full" : "message_database_error", 503);
    return r == SQLITE_ROW;
  }
  std::string text(int n) {
    auto p = sqlite3_column_text(s, n);
    return p ? reinterpret_cast<const char *>(p) : "";
  }
  std::int64_t number(int n) { return sqlite3_column_int64(s, n); }
};
std::string str(const Json &p, const char *k, std::size_t max = 128) {
  if (!p.contains(k) || !p[k].is_string() || p[k].get_ref<const std::string &>().empty() ||
      p[k].get_ref<const std::string &>().size() > max)
    throw Failure("invalid_message_contract");
  return p[k];
}
std::int64_t number(const Json &p, const char *k) {
  auto s = str(p, k, 18);
  if (!std::regex_match(s, std::regex("0|[1-9][0-9]{0,17}")))
    throw Failure("invalid_message_counter");
  return std::stoll(s);
}
void fields(const Json &p, std::initializer_list<std::string_view> allowed) {
  if (!p.is_object())
    throw Failure("invalid_message_contract");
  for (auto &[k, v] : p.items())
    if (std::find(allowed.begin(), allowed.end(), k) == allowed.end())
      throw Failure("invalid_message_field");
}
std::int64_t scalar(sqlite3 *d, const char *s) {
  Q q(d, s);
  q.next();
  return q.number(0);
}
} // namespace
Json validate(const Json &r) {
  if (r.dump().size() > 3500)
    throw Failure("message_report_limit");
  fields(r, {"apiVersion", "epoch", "clock", "payloadRecorded", "total", "evicted", "events"});
  if (r.value("apiVersion", "") != "graphlab.message-observations/v1" ||
      r.value("clock", "") != "reporter-monotonic-ns" || !r.contains("payloadRecorded") ||
      r["payloadRecorded"] != false ||
      !std::regex_match(str(r, "epoch"), std::regex("[a-f0-9]{32}")) || !r.contains("events") ||
      !r["events"].is_array() || r["events"].size() > 8)
    throw Failure("invalid_message_contract");
  auto total = number(r, "total"), evicted = number(r, "evicted");
  if (total < evicted || total - evicted != static_cast<std::int64_t>(r["events"].size()))
    throw Failure("message_window_invalid");
  auto expected = evicted + 1;
  std::int64_t previous_time = 0;
  for (const auto &e : r["events"]) {
    fields(e, {"sequence", "messageId", "traceId", "stream", "kind", "payloadLength",
               "timestampMonotonicNs", "phase"});
    if (number(e, "sequence") != expected++ ||
        !std::regex_match(str(e, "messageId"), std::regex("[a-f0-9]{48}")) ||
        e.at("traceId") != e["messageId"] ||
        (e.value("stream", "") != "alpha" && e.value("stream", "") != "beta") ||
        (e.value("kind", "") != "send" && e.value("kind", "") != "receive") ||
        (e.value("phase", "") != "request" && e.value("phase", "") != "response") ||
        number(e, "payloadLength") != 53)
      throw Failure("invalid_message_event");
    auto time = number(e, "timestampMonotonicNs");
    if (time < previous_time)
      throw Failure("message_clock_regressed");
    previous_time = time;
  }
  return r;
}
struct History::Impl {
  sqlite3 *db = nullptr;
  std::mutex mutex;
  std::condition_variable condition;
  bool stop = false;
  std::thread thread;
  std::string error, collector = console::random_hex(16);
  explicit Impl(const std::filesystem::path &p) {
    if (std::filesystem::is_symlink(p))
      throw Failure("unsafe_message_database");
    if (sqlite3_open(p.c_str(), &db) != SQLITE_OK) {
      sqlite3_close(db);
      throw Failure("message_database_open", 503);
    }
    chmod(p.c_str(), 0600);
    sqlite3_busy_timeout(db, 100);
    try {
      sql(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA max_page_count=4096; "
              "CREATE TABLE IF NOT EXISTS sources(k TEXT PRIMARY KEY,run TEXT,node TEXT,instance "
              "TEXT,epoch TEXT,high INTEGER,missed INTEGER,evicted INTEGER,checked "
              "INTEGER,collector TEXT); CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY "
              "AUTOINCREMENT,k TEXT,seq INTEGER,stream TEXT,created INTEGER,body "
              "TEXT,UNIQUE(k,seq)); CREATE INDEX IF NOT EXISTS events_scope ON events(k,id);");
    } catch (...) {
      sqlite3_close(db);
      throw;
    }
  }
  ~Impl() {
    {
      std::lock_guard g(mutex);
      stop = true;
      condition.notify_all();
    }
    if (thread.joinable())
      thread.join();
    sqlite3_close(db);
  }
  void prune(std::int64_t now) {
    for (;;) {
      Q q(db, "SELECT id FROM events WHERE created<? OR (SELECT COUNT(*) FROM events)>4096 OR "
              "(SELECT COALESCE(SUM(length(body)),0) FROM events)>4194304 ORDER BY id LIMIT 1");
      q.num(1, now - 86400);
      if (!q.next())
        break;
      Q d(db, "DELETE FROM events WHERE id=?");
      d.num(1, q.number(0));
      d.next();
    }
  }
};
History::History(const std::filesystem::path &p) : impl_(std::make_unique<Impl>(p)) {}
History::~History() = default;
void History::ingest(const Json &c, const Json &input, std::int64_t now) {
  auto run = str(c, "runId"), node = str(c, "node"), instance = str(c, "instance");
  auto r = validate(input);
  auto epoch = str(r, "epoch");
  auto key = lab_support::digest(Json::array({run, node, instance, epoch}));
  if (!now)
    now = std::time(nullptr);
  auto &i = *impl_;
  std::lock_guard g(i.mutex);
  sql(i.db, "BEGIN IMMEDIATE");
  try {
    Q old(i.db, "SELECT high,missed FROM sources WHERE k=?");
    old.str(1, key);
    bool found = old.next();
    auto high = found ? old.number(0) : 0, missed = found ? old.number(1) : 0;
    auto total = number(r, "total"), evicted = number(r, "evicted");
    if (total < high)
      throw Failure("message_report_regressed");
    if (!found && scalar(i.db, "SELECT COUNT(*) FROM sources") >= 256)
      throw Failure("message_source_capacity", 429);
    missed += std::max<std::int64_t>(0, evicted - high);
    for (const auto &e : r["events"]) {
      auto seq = number(e, "sequence");
      if (seq <= high) {
        Q q(i.db, "SELECT body FROM events WHERE k=? AND seq=?");
        q.str(1, key);
        q.num(2, seq);
        if (q.next() && Json::parse(q.text(0))["observation"] != e)
          throw Failure("message_sequence_conflict");
        continue;
      }
      Json body = {{"apiVersion", "graphlab.message-event/v1"},
                   {"runId", run},
                   {"node", node},
                   {"workloadInstance", instance},
                   {"reporterEpoch", epoch},
                   {"clock", r["clock"]},
                   {"observation", e},
                   {"observedAt", console::timestamp()},
                   {"collectorEpoch", i.collector},
                   {"source", "identity-checked-workload-report"},
                   {"deliveryAccounting", "unavailable"},
                   {"payloadRecorded", false}};
      Q add(i.db, "INSERT INTO events(k,seq,stream,created,body) VALUES(?,?,?,?,?)");
      add.str(1, key);
      add.num(2, seq);
      add.str(3, e["stream"]);
      add.num(4, now);
      add.str(5, body.dump());
      add.next();
    }
    Q source(i.db, "INSERT INTO sources VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(k) DO UPDATE SET "
                   "high=excluded.high,missed=excluded.missed,evicted=excluded.evicted,checked="
                   "excluded.checked,collector=excluded.collector");
    source.str(1, key);
    source.str(2, run);
    source.str(3, node);
    source.str(4, instance);
    source.str(5, epoch);
    source.num(6, total);
    source.num(7, missed);
    source.num(8, evicted);
    source.num(9, now);
    source.str(10, i.collector);
    source.next();
    i.prune(now);
    sql(i.db, "COMMIT");
  } catch (...) {
    sqlite3_exec(i.db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
void History::start(runtime::Backend &backend, std::function<Json()> candidates) {
  auto &i = *impl_;
  i.thread = std::thread([this, &backend, candidates = std::move(candidates)] {
    auto &i = *impl_;
    for (;;) {
      {
        std::unique_lock g(i.mutex);
        if (i.condition.wait_for(g, std::chrono::seconds(5), [&] { return i.stop; }))
          return;
      }
      std::string errors;
      try {
        for (const auto &item : candidates()) {
          {
            std::lock_guard g(i.mutex);
            if (i.stop)
              return;
          }
          try {
            const auto &r = item["resource"];
            auto report = backend.message_report(item["run"], r);
            if (!report.is_null())
              ingest({{"runId", item["run"]["id"]},
                      {"node", r["logical"]},
                      {"instance", r["identity"]["id"]}},
                     report);
          } catch (const std::exception &e) {
            errors = std::string(e.what()).substr(0, 256);
          }
        }
      } catch (const std::exception &e) {
        errors = std::string(e.what()).substr(0, 256);
      }
      {
        std::lock_guard g(i.mutex);
        i.error = errors;
        try {
          i.prune(std::time(nullptr));
        } catch (const std::exception &e) {
          i.error = std::string(e.what()).substr(0, 256);
        }
      }
    }
  });
}
Json History::query(const Json &p) {
  fields(p, {"runId", "node", "stream", "cursor"});
  auto run = str(p, "runId"), node = str(p, "node");
  std::string stream = p.contains("stream") ? str(p, "stream") : "";
  if (!stream.empty() && stream != "alpha" && stream != "beta")
    throw Failure("message_stream_invalid");
  auto scope = lab_support::digest(Json::array({run, node, stream}));
  std::int64_t before = INT64_MAX;
  if (p.contains("cursor")) {
    auto c = str(p, "cursor", 256);
    if (!c.starts_with(scope + "."))
      throw Failure("message_cursor_scope");
    before = number(Json{{"n", c.substr(scope.size() + 1)}}, "n");
  }
  auto &i = *impl_;
  std::lock_guard g(i.mutex);
  Json items = Json::array(), sources = Json::array();
  Q q(i.db, "SELECT e.id,e.body FROM events e JOIN sources s ON s.k=e.k WHERE s.run=? AND s.node=? "
            "AND (?='' OR e.stream=?) AND e.id<? AND e.created>=? ORDER BY e.id DESC LIMIT 21");
  q.str(1, run);
  q.str(2, node);
  q.str(3, stream);
  q.str(4, stream);
  q.num(5, before);
  q.num(6, std::time(nullptr) - 86400);
  while (q.next()) {
    auto b = Json::parse(q.text(1));
    b["id"] = std::to_string(q.number(0));
    items.push_back(b);
  }
  Json cursor = nullptr;
  if (items.size() > 20) {
    items.erase(20);
    cursor = scope + "." + items.back()["id"].get<std::string>();
  }
  Q s(i.db, "SELECT k,instance,epoch,high,missed,evicted,checked,collector FROM sources WHERE "
            "run=? AND node=?");
  s.str(1, run);
  s.str(2, node);
  while (s.next()) {
    Q count(i.db, "SELECT COUNT(*) FROM events WHERE k=? AND created>=?");
    count.str(1, s.text(0));
    count.num(2, std::time(nullptr) - 86400);
    count.next();
    sources.push_back(
        {{"instance", s.text(1)},
         {"epoch", s.text(2)},
         {"total", std::to_string(s.number(3))},
         {"missedBeforeIngestion", std::to_string(s.number(4))},
         {"reporterEvicted", std::to_string(s.number(5))},
         {"retained", std::to_string(count.number(0))},
         {"expiredOrPruned", std::to_string(s.number(3) - s.number(4) - count.number(0))},
         {"stale", std::time(nullptr) - s.number(6) > 15 || s.text(7) != i.collector}});
  }
  return {{"items", items},
          {"sources", sources},
          {"nextCursor", cursor},
          {"error", i.error},
          {"sourceCapacityReached", scalar(i.db, "SELECT COUNT(*) FROM sources") >= 256},
          {"deliveryAccounting", "unavailable: omitted observations are not delivery loss"}};
}
Json History::event(const Json &p) {
  fields(p, {"runId", "node", "id", "edge"});
  auto run = str(p, "runId"), node = str(p, "node");
  auto id = number(p, "id");
  auto &i = *impl_;
  std::lock_guard g(i.mutex);
  Q q(i.db, "SELECT e.body FROM events e JOIN sources s ON s.k=e.k WHERE e.id=? AND s.run=? AND "
            "s.node=? AND e.created>=?");
  q.num(1, id);
  q.str(2, run);
  q.str(3, node);
  q.num(4, std::time(nullptr) - 86400);
  if (!q.next())
    throw Failure("message_event_not_retained", 404);
  auto b = Json::parse(q.text(0));
  b["id"] = std::to_string(id);
  return b;
}
} // namespace graphlab::messages
