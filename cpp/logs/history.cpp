#include <ctime>
#include <graphlab/process_logs.hpp>
#include <graphlab/terminal.hpp>
#include <openssl/evp.h>
#include <regex>
#include <sqlite3.h>
#include <sys/stat.h>
namespace graphlab::process_logs {
using runtime::Failure;
namespace {
constexpr std::int64_t global_bytes = 32 * 1024 * 1024, source_bytes = 4 * 1024 * 1024;
void sql(sqlite3 *d, const char *s) {
  if (sqlite3_exec(d, s, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw Failure(sqlite3_errcode(d) == SQLITE_FULL ? "log_storage_full" : "log_database_error",
                  503);
}
struct Q {
  sqlite3_stmt *s = nullptr;
  Q(sqlite3 *d, const char *q) {
    if (sqlite3_prepare_v2(d, q, -1, &s, nullptr) != SQLITE_OK)
      throw Failure("log_database_error", 503);
  }
  ~Q() { sqlite3_finalize(s); }
  void str(int i, const std::string &v) {
    sqlite3_bind_text(s, i, v.data(), v.size(), SQLITE_TRANSIENT);
  }
  void num(int i, std::int64_t v) { sqlite3_bind_int64(s, i, v); }
  bool next() {
    int n = sqlite3_step(s);
    if (n != SQLITE_ROW && n != SQLITE_DONE)
      throw Failure(n == SQLITE_FULL ? "log_storage_full" : "log_database_error", 503);
    return n == SQLITE_ROW;
  }
  std::string text(int i) {
    auto p = sqlite3_column_text(s, i);
    return p ? reinterpret_cast<const char *>(p) : "";
  }
  std::int64_t number(int i) { return sqlite3_column_int64(s, i); }
};
std::int64_t scalar(sqlite3 *d, const char *s) {
  Q q(d, s);
  q.next();
  return q.number(0);
}
std::string digest(std::string_view b) {
  unsigned char hash[32];
  unsigned n = 0;
  if (!EVP_Digest(b.data(), b.size(), hash, &n, EVP_sha256(), nullptr))
    throw Failure("log_hash_failed");
  std::string s = "sha256:";
  for (auto c : hash) {
    s += "0123456789abcdef"[c >> 4];
    s += "0123456789abcdef"[c & 15];
  }
  return s;
}
std::string field(const Json &p, const char *k) {
  if (!p.contains(k) || !p[k].is_string() || p[k].get<std::string>().empty() ||
      p[k].get<std::string>().size() > 256)
    throw Failure("invalid_log_scope");
  return p[k];
}
void fields(const Json &p, std::initializer_list<std::string_view> allowed) {
  if (!p.is_object() || p.dump().size() > 2048)
    throw Failure("invalid_log_query");
  for (auto &[k, v] : p.items())
    if (std::find(allowed.begin(), allowed.end(), k) == allowed.end())
      throw Failure("invalid_log_query");
}
std::int64_t integer(const std::string &s) {
  if (!std::regex_match(s, std::regex("[1-9][0-9]{0,17}")))
    throw Failure("invalid_log_cursor");
  return std::stoll(s);
}
} // namespace
struct History::Impl {
  sqlite3 *db = nullptr;
  std::mutex mutex;
  std::condition_variable condition;
  bool stop = false;
  std::thread worker;
  std::string collector = console::random_hex(16), error;
  explicit Impl(const std::filesystem::path &p) {
    if (std::filesystem::is_symlink(p))
      throw Failure("unsafe_log_database");
    if (sqlite3_open(p.c_str(), &db) != SQLITE_OK) {
      sqlite3_close(db);
      db = nullptr;
      throw Failure("log_database_open", 503);
    }
    chmod(p.c_str(), 0600);
    sqlite3_busy_timeout(db, 100);
    try {
      sql(db,
          "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA max_page_count=16384; "
          "CREATE TABLE IF NOT EXISTS log_sources(k TEXT PRIMARY KEY,run TEXT,node TEXT,source "
          "TEXT,generation TEXT,last_hash TEXT,last_id TEXT,collector TEXT,checked INTEGER,error "
          "TEXT,evicted INTEGER DEFAULT 0); CREATE TABLE IF NOT EXISTS log_artifacts(seq INTEGER "
          "PRIMARY KEY AUTOINCREMENT,k TEXT,created INTEGER,body TEXT,payload BLOB); CREATE INDEX "
          "IF NOT EXISTS log_by_source ON log_artifacts(k,seq);");
    } catch (...) {
      sqlite3_close(db);
      db = nullptr;
      throw;
    }
  }
  ~Impl() {
    {
      std::lock_guard g(mutex);
      stop = true;
      condition.notify_all();
    }
    if (worker.joinable())
      worker.join();
    if (db)
      sqlite3_close(db);
  }
  void prune(std::int64_t now) {
    // Prune oldest artifacts until all logical quotas hold; source ledgers preserve visible
    // omissions.
    for (;;) {
      Q q(db, "SELECT seq,k FROM log_artifacts WHERE created<? OR k IN (SELECT k FROM "
              "log_artifacts GROUP BY k HAVING SUM(length(payload))>4194304 OR COUNT(*)>128) OR "
              "(SELECT COALESCE(SUM(length(payload)),0) FROM log_artifacts)>33554432 OR (SELECT "
              "COUNT(*) FROM log_artifacts)>512 ORDER BY seq LIMIT 1");
      q.num(1, now - 86400);
      if (!q.next())
        break;
      auto id = q.number(0);
      auto k = q.text(1);
      Q d(db, "DELETE FROM log_artifacts WHERE seq=?");
      d.num(1, id);
      d.next();
      Q u(db, "UPDATE log_sources SET evicted=evicted+1 WHERE k=?");
      u.str(1, k);
      u.next();
    }
  }
};
History::History(const std::filesystem::path &p) : impl_(std::make_unique<Impl>(p)) {}
History::~History() = default;
void History::ingest(const Json &context, const Json &snapshot, std::int64_t now) {
  auto &i = *impl_;
  if (!now)
    now = std::time(nullptr);
  auto run = field(context, "runId"), node = field(context, "node"),
       source = field(context, "source");
  auto generation = field(snapshot, "generation");
  if (!snapshot.contains("base64") || !snapshot["base64"].is_string() ||
      snapshot["base64"].get_ref<const std::string &>().size() > 90000)
    throw Failure("log_snapshot_limit");
  auto bytes = terminal::decode(snapshot["base64"]);
  // base64 is separately bounded below; empty output is a valid observation.
  if (bytes.size() > 65536)
    throw Failure("log_snapshot_limit");
  auto key = lab_support::digest(Json::array({run, node, source})), hash = digest(bytes);
  Json meta = snapshot;
  meta.erase("base64");
  if (meta.dump().size() > 2048)
    throw Failure("log_metadata_limit");
  std::lock_guard g(i.mutex);
  sql(i.db, "BEGIN IMMEDIATE");
  try {
    Q prior(i.db, "SELECT generation,last_hash,last_id,collector FROM log_sources WHERE k=?");
    prior.str(1, key);
    bool found = prior.next();
    if (!found && scalar(i.db, "SELECT COUNT(*) FROM log_sources") >= 256)
      throw Failure("log_source_capacity", 429);
    bool retained = false;
    if (found) {
      Q exists(i.db, "SELECT 1 FROM log_artifacts WHERE seq=? AND created>=?");
      exists.str(1, prior.text(2));
      exists.num(2, now - 86400);
      retained = exists.next();
    }
    bool changed = !found || prior.text(0) != generation || prior.text(1) != hash ||
                   prior.text(3) != i.collector || !retained;
    std::string id = found ? prior.text(2) : "";
    if (changed) {
      meta["apiVersion"] = "graphlab.process-log-snapshot/v1";
      meta["runId"] = run;
      meta["node"] = node;
      meta["sourceId"] = source;
      meta["sha256"] = hash;
      meta["size"] = std::to_string(bytes.size());
      meta["finalizedAt"] = console::timestamp();
      meta["collectorEpoch"] = i.collector;
      meta["coverage"] =
          "bounded-runtime-tail; discontinuous snapshots; intervening bytes may be omitted";
      meta["gap"] = !found                         ? "first_observation"
                    : prior.text(0) != generation  ? "source_generation_changed"
                    : prior.text(3) != i.collector ? "collector_restarted"
                                                   : "snapshot_boundary";
      Q a(i.db, "INSERT INTO log_artifacts(k,created,body,payload) VALUES(?,?,?,?)");
      a.str(1, key);
      a.num(2, now);
      a.str(3, meta.dump());
      sqlite3_bind_blob(a.s, 4, bytes.data(), bytes.size(), SQLITE_TRANSIENT);
      a.next();
      id = std::to_string(sqlite3_last_insert_rowid(i.db));
    }
    Q s(i.db, "INSERT INTO "
              "log_sources(k,run,node,source,generation,last_hash,last_id,collector,checked,error) "
              "VALUES(?,?,?,?,?,?,?,?,?,'') ON CONFLICT(k) DO UPDATE SET "
              "generation=excluded.generation,last_hash=excluded.last_hash,last_id=excluded.last_"
              "id,collector=excluded.collector,checked=excluded.checked,error=''");
    s.str(1, key);
    s.str(2, run);
    s.str(3, node);
    s.str(4, source);
    s.str(5, generation);
    s.str(6, hash);
    s.str(7, id);
    s.str(8, i.collector);
    s.num(9, now);
    s.next();
    i.prune(now);
    sql(i.db, "COMMIT");
    i.error.clear();
  } catch (...) {
    sqlite3_exec(i.db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
void History::start(runtime::Backend &backend, std::function<Json()> candidates) {
  auto &i = *impl_;
  i.worker = std::thread([this, &backend, candidates = std::move(candidates)] {
    auto &i = *impl_;
    for (;;) {
      {
        std::unique_lock g(i.mutex);
        if (i.condition.wait_for(g, std::chrono::seconds(5), [&] { return i.stop; }))
          return;
      }
      try {
        {
          std::lock_guard g(i.mutex);
          sql(i.db, "BEGIN IMMEDIATE");
          try {
            i.prune(std::time(nullptr));
            sql(i.db, "COMMIT");
          } catch (...) {
            sqlite3_exec(i.db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
          }
        }
        auto list = candidates();
        for (const auto &item : list) {
          {
            std::lock_guard g(i.mutex);
            if (i.stop)
              return;
          }
          const auto &run = item["run"];
          const auto &r = item["resource"];
          Json context = {{"runId", run["id"]}, {"node", r["logical"]}, {"source", r["logSource"]}};
          try {
            ingest(context, backend.logs(run, r));
          } catch (const std::exception &e) {
            std::lock_guard g(i.mutex);
            i.error = std::string(e.what()).substr(0, 256);
            try {
              auto key = lab_support::digest(
                  Json::array({context["runId"], context["node"], context["source"]}));
              if (scalar(i.db, "SELECT COUNT(*) FROM log_sources") < 256) {
                Q add(i.db, "INSERT OR IGNORE INTO "
                            "log_sources(k,run,node,source,generation,last_hash,last_id,collector,"
                            "checked,error) VALUES(?,?,?,?,'unknown','','','',0,'')");
                add.str(1, key);
                add.str(2, context["runId"]);
                add.str(3, context["node"]);
                add.str(4, context["source"]);
                add.next();
              }
              Q q(i.db,
                  "UPDATE log_sources SET error=?,checked=? WHERE run=? AND node=? AND source=?");
              q.str(1, i.error);
              q.num(2, std::time(nullptr));
              q.str(3, context["runId"]);
              q.str(4, context["node"]);
              q.str(5, context["source"]);
              q.next();
            } catch (...) {
            }
          }
        }
      } catch (const std::exception &e) {
        std::lock_guard g(i.mutex);
        i.error = std::string(e.what()).substr(0, 256);
      }
    }
  });
}
Json History::query(const Json &p) {
  fields(p, {"runId", "node", "source", "cursor"});
  auto run = field(p, "runId"), node = field(p, "node");
  std::string source = p.value("source", "");
  if (source.size() > 64)
    throw Failure("invalid_log_scope");
  std::int64_t before = INT64_MAX;
  auto scope = lab_support::digest(Json::array({run, node, source}));
  if (p.contains("cursor")) {
    auto c = p["cursor"].get<std::string>();
    auto dot = c.find('.');
    if (dot == std::string::npos || c.substr(0, dot) != scope)
      throw Failure("log_cursor_scope");
    before = integer(c.substr(dot + 1));
  }
  auto &i = *impl_;
  std::lock_guard g(i.mutex);
  Json items = Json::array(), sources = Json::array();
  Q q(i.db, "SELECT a.seq,a.body FROM log_artifacts a JOIN log_sources s ON s.k=a.k WHERE s.run=? "
            "AND s.node=? AND (?='' OR s.source=?) AND a.seq<? AND a.created>=? ORDER BY a.seq "
            "DESC LIMIT 21");
  q.str(1, run);
  q.str(2, node);
  q.str(3, source);
  q.str(4, source);
  q.num(5, before);
  q.num(6, std::time(nullptr) - 86400);
  while (q.next()) {
    auto body = Json::parse(q.text(1));
    body["id"] = std::to_string(q.number(0));
    items.push_back(body);
  }
  Json next = nullptr;
  if (items.size() > 20) {
    items.erase(20);
    next = scope + "." + items.back()["id"].get<std::string>();
  }
  Q s(i.db, "SELECT source,generation,checked,error,evicted,collector FROM log_sources WHERE run=? "
            "AND node=? ORDER BY source");
  s.str(1, run);
  s.str(2, node);
  while (s.next())
    sources.push_back({{"id", s.text(0)},
                       {"generation", s.text(1)},
                       {"checkedUnixSeconds", std::to_string(s.number(2))},
                       {"stale", std::time(nullptr) - s.number(2) > 15 || !s.text(3).empty() ||
                                     s.text(5) != i.collector},
                       {"error", s.text(3)},
                       {"evicted", std::to_string(s.number(4))}});
  return {{"items", items},
          {"sources", sources},
          {"nextCursor", next},
          {"error", i.error},
          {"sourceCapacityReached", scalar(i.db, "SELECT COUNT(*) FROM log_sources") >= 256},
          {"coverage", "discontinuous bounded tail snapshots; runtime rotation and between-poll "
                       "output may be omitted"}};
}
Json History::download(const Json &p) {
  fields(p, {"runId", "node", "id"});
  auto run = field(p, "runId"), node = field(p, "node"), id = field(p, "id");
  integer(id);
  auto &i = *impl_;
  std::lock_guard g(i.mutex);
  Q q(i.db, "SELECT a.body,a.payload FROM log_artifacts a JOIN log_sources s ON s.k=a.k WHERE "
            "s.run=? AND s.node=? AND a.seq=? AND a.created>=?");
  q.str(1, run);
  q.str(2, node);
  q.str(3, id);
  q.num(4, std::time(nullptr) - 86400);
  if (!q.next())
    throw Failure("log_artifact_not_retained", 404);
  auto meta = Json::parse(q.text(0));
  auto n = sqlite3_column_bytes(q.s, 1);
  if (n > 65536)
    throw Failure("log_artifact_size_changed");
  auto data = static_cast<const char *>(sqlite3_column_blob(q.s, 1));
  std::string bytes(data ? data : "", n);
  if (meta["sha256"] != digest(bytes) || meta["size"] != std::to_string(n))
    throw Failure("log_artifact_checksum_mismatch", 409);
  meta["id"] = id;
  meta["base64"] = terminal::encode(bytes);
  return meta;
}
} // namespace graphlab::process_logs
