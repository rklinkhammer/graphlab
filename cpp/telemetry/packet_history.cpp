#include <charconv>
#include <fcntl.h>
#include <graphlab/capture.hpp>
#include <graphlab/packet_history.hpp>
#include <graphlab/telemetry.hpp>
#include <map>
#include <openssl/evp.h>
#include <regex>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
namespace graphlab::packets {
namespace {
constexpr std::uint64_t max_file = 128ull * 1024 * 1024;
[[noreturn]] void database_error(sqlite3 *db) {
  throw runtime::Failure(
      sqlite3_errcode(db) == SQLITE_FULL ? "packet_storage_full" : "packet_database_error", 503);
}
void sql(sqlite3 *db, const char *s) {
  if (sqlite3_exec(db, s, nullptr, nullptr, nullptr) != SQLITE_OK)
    database_error(db);
}
struct Query {
  sqlite3_stmt *s = nullptr;
  Query(sqlite3 *db, const char *q) {
    if (sqlite3_prepare_v2(db, q, -1, &s, nullptr) != SQLITE_OK)
      database_error(db);
  }
  ~Query() { sqlite3_finalize(s); }
  void str(int i, const std::string &v) {
    sqlite3_bind_text(s, i, v.data(), v.size(), SQLITE_TRANSIENT);
  }
  void num(int i, sqlite3_int64 v) { sqlite3_bind_int64(s, i, v); }
  bool next() {
    auto n = sqlite3_step(s);
    if (n != SQLITE_ROW && n != SQLITE_DONE)
      database_error(sqlite3_db_handle(s));
    return n == SQLITE_ROW;
  }
  std::string text(int i = 0) { return reinterpret_cast<const char *>(sqlite3_column_text(s, i)); }
  sqlite3_int64 number(int i = 0) { return sqlite3_column_int64(s, i); }
};
sqlite3_int64 scalar(sqlite3 *db, const char *s) {
  Query q(db, s);
  q.next();
  return q.number();
}
std::uint64_t integer(const Json &v) {
  if (!v.is_string())
    throw runtime::Failure("invalid_packet_integer");
  auto s = v.get<std::string>();
  std::uint64_t n = 0;
  auto [end, e] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (s.empty() || s.size() > 19 || (s.size() > 1 && s[0] == '0') || e != std::errc{} ||
      end != s.data() + s.size() || n > INT64_MAX)
    throw runtime::Failure("invalid_packet_integer");
  return n;
}
std::vector<unsigned char> verified(const std::filesystem::path &path, const Json &segment) {
  auto size = integer(segment.at("size"));
  if (size > max_file)
    throw runtime::Failure("segment_byte_limit");
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    throw runtime::Failure("packet_artifact_unavailable");
  struct Guard {
    int fd;
    ~Guard() { close(fd); }
  } guard{fd};
  struct stat st{};
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || std::uint64_t(st.st_size) != size)
    throw runtime::Failure("packet_artifact_size_changed");
  std::vector<unsigned char> bytes(size);
  std::size_t offset = 0;
  while (offset < size) {
    auto n = read(fd, bytes.data() + offset, size - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw runtime::Failure("packet_artifact_read");
    offset += n;
  }
  unsigned char hash[32];
  unsigned length = 0;
  if (!EVP_Digest(bytes.data(), bytes.size(), hash, &length, EVP_sha256(), nullptr))
    throw runtime::Failure("packet_artifact_hash");
  std::string digest = "sha256:";
  for (auto b : hash) {
    digest += "0123456789abcdef"[b >> 4];
    digest += "0123456789abcdef"[b & 15];
  }
  if (digest != segment.at("sha256").get<std::string>())
    throw runtime::Failure("packet_artifact_checksum_mismatch");
  return bytes;
}
} // namespace
struct History::Impl {
  sqlite3 *db = nullptr;
  std::mutex mutex;
  std::condition_variable condition;
  bool stop = false, busy = false;
  std::optional<Json> pending;
  std::thread worker;
  std::string error, epoch, active_run;
  std::uint64_t retired_in_scan = 0;
  bool window_truncated = false;
  explicit Impl(const std::filesystem::path &path) {
    if (std::filesystem::exists(path.string() + ".recovering"))
      throw runtime::Failure("packet_recovery_interrupted", 503);
    if (std::filesystem::is_symlink(path))
      throw runtime::Failure("packet_database_symlink");
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
      sqlite3_close(db);
      throw runtime::Failure("packet_database_open");
    }
    chmod(path.c_str(), 0600);
    try {
      sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA "
              "max_page_count=16384; PRAGMA cache_size=-2048;"
              "CREATE TABLE IF NOT EXISTS packet_meta(id INTEGER PRIMARY KEY CHECK(id=1),epoch "
              "TEXT,generation INTEGER);"
              "CREATE TABLE IF NOT EXISTS packet_segments(run TEXT,artifact TEXT,body TEXT,PRIMARY "
              "KEY(run,artifact));"
              "CREATE TABLE IF NOT EXISTS packet_rows(id INTEGER PRIMARY KEY AUTOINCREMENT,run "
              "TEXT,artifact TEXT,edge TEXT,node_a TEXT,node_b TEXT,protocol TEXT,time "
              "INTEGER,body TEXT);"
              "CREATE INDEX IF NOT EXISTS packet_scope ON packet_rows(run,id);"
              "CREATE TABLE IF NOT EXISTS packet_retirement(id INTEGER PRIMARY KEY "
              "CHECK(id=1),floor TEXT,retired INTEGER);"
              "INSERT OR IGNORE INTO packet_retirement VALUES(1,'',0);"
              "CREATE TABLE IF NOT EXISTS packet_maintenance(id INTEGER PRIMARY KEY "
              "CHECK(id=1),body TEXT);");
      bool key_column = false;
      {
        Query q(db, "PRAGMA table_info(packet_segments)");
        while (q.next())
          if (q.text(1) == "order_key")
            key_column = true;
      }
      if (!key_column)
        sql(db, "ALTER TABLE packet_segments ADD COLUMN order_key TEXT NOT NULL DEFAULT ''");
      sql(db, "CREATE INDEX IF NOT EXISTS packet_segment_order ON packet_segments(order_key)");
      {
        Query q(db, "SELECT body FROM packet_maintenance WHERE id=1");
        if (q.next()) {
          auto m = Json::parse(q.text());
          if (m["state"] == "queued" || m["state"] == "running") {
            m["state"] = "interrupted";
            maintenance(m);
          }
        }
      }
      {
        Query q(db, "INSERT OR IGNORE INTO packet_meta VALUES(1,?,0)");
        q.str(1, console::random_hex(16));
        q.next();
      }
      {
        Query q(db, "SELECT epoch FROM packet_meta");
        q.next();
        epoch = q.text();
      }
#ifdef GRAPHLAB_TEST_CHECKPOINTS
      if (const auto *pages = std::getenv("GRAPHLAB_PACKET_TEST_PAGES")) {
        auto n = integer(std::string(pages));
        if (n < 16 || n > 16384)
          throw runtime::Failure("invalid_test_pages");
        sql(db, ("PRAGMA max_page_count=" + std::to_string(n)).c_str());
      }
#endif
      worker = std::thread([this] { work(); });
    } catch (...) {
      sqlite3_close(db);
      throw;
    }
  }
  ~Impl() {
    {
      std::lock_guard l(mutex);
      stop = true;
      condition.notify_all();
    }
    if (worker.joinable())
      worker.join();
    sqlite3_close(db);
  }
  void maintenance(const Json &value) {
    Query q(db, "INSERT OR REPLACE INTO packet_maintenance VALUES(1,?)");
    q.str(1, value.dump());
    q.next();
  }
  Json maintenance() {
    Query q(db, "SELECT body FROM packet_maintenance WHERE id=1");
    return q.next() ? Json::parse(q.text()) : Json(nullptr);
  }
  std::string floor() {
    Query q(db, "SELECT floor FROM packet_retirement WHERE id=1");
    q.next();
    return q.text();
  }
  void recycle() {
    // Legacy v1 receipts remain pinned until a registered manifest supplies their ordering key.
    Query boundary(db, "SELECT order_key FROM packet_segments WHERE order_key<>'' ORDER BY "
                       "order_key DESC LIMIT 1 OFFSET 1000");
    if (!boundary.next())
      return;
    auto key = boundary.text();
    Query update(db,
                 "UPDATE packet_retirement SET floor=max(floor,?),retired=retired+(SELECT count(*) "
                 "FROM packet_segments WHERE order_key<>'' AND order_key<=?) WHERE id=1");
    update.str(1, key);
    update.str(2, key);
    update.next();
    Query remove(db, "DELETE FROM packet_segments WHERE order_key<>'' AND order_key<=?");
    remove.str(1, key);
    remove.next();
  }
  void prune() {
    Query q(db, "DELETE FROM packet_rows WHERE time<?");
    q.num(1, telemetry::wall() - 86400);
    q.next();
    bool changed = sqlite3_changes(db) > 0;
    sql(db, "DELETE FROM packet_rows WHERE id IN (SELECT id FROM packet_rows ORDER BY id DESC "
            "LIMIT -1 OFFSET 20000)");
    changed |= sqlite3_changes(db) > 0;
    // Bounded encoded packet bodies, independently of SQLite's hard main-file ceiling.
    while (scalar(db, "SELECT coalesce(sum(length(CAST(body AS BLOB))),0) FROM packet_rows") >
           16 * 1024 * 1024) {
      sql(db, "DELETE FROM packet_rows WHERE id IN (SELECT id FROM packet_rows ORDER BY id LIMIT "
              "2000)");
      changed = true;
    }
    if (changed)
      sql(db, "UPDATE packet_meta SET generation=generation+1");
  }
  bool known(const std::string &run, const std::string &artifact, const std::string &key,
             bool rebuild) {
    std::lock_guard l(mutex);
    bool found = false;
    bool legacy = false;
    {
      Query q(db, "SELECT order_key FROM packet_segments WHERE run=? AND artifact=?");
      q.str(1, run);
      q.str(2, artifact);
      found = q.next();
      if (found)
        legacy = q.text().empty();
    }
    if (found) {
      if (!legacy)
        return true;
      sql(db, "BEGIN IMMEDIATE");
      try {
        Query q(
            db,
            "UPDATE packet_segments SET order_key=? WHERE run=? AND artifact=? AND order_key=''");
        q.str(1, key);
        q.str(2, run);
        q.str(3, artifact);
        q.next();
        recycle();
        sql(db, "COMMIT");
      } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
      }
      return true;
    }
    if (!rebuild && key <= floor()) {
      ++retired_in_scan;
      return true;
    }
    return false;
  }
  void save(const Json &run, const Json &c, const std::string &artifact, const std::string &key,
            Json status, Json rows) {
    std::lock_guard l(mutex);
    sql(db, "BEGIN IMMEDIATE");
    try {
      status["artifactId"] = artifact;
      status["edge"] = c.at("edge");
      Query s(db, "INSERT INTO packet_segments(run,artifact,body,order_key) VALUES(?,?,?,?)");
      s.str(1, run.at("id"));
      s.str(2, artifact);
      s.str(3, status.dump());
      s.str(4, key);
      s.next();
      std::string a, b;
      for (const auto &e : run.at("topology").at("edges"))
        if (e.at("id") == c.at("edge")) {
          a = e["endpoints"][0].get<std::string>();
          b = e["endpoints"][1].get<std::string>();
          a = a.substr(0, a.find(':'));
          b = b.substr(0, b.find(':'));
        }
      for (const auto &row : rows) {
        auto body = row.dump();
        if (body.size() > 2048)
          throw runtime::Failure("packet_record_byte_limit");
        Query p(db, "INSERT INTO packet_rows(run,artifact,edge,node_a,node_b,protocol,time,body) "
                    "VALUES(?,?,?,?,?,?,?,?)");
        p.str(1, run.at("id"));
        p.str(2, artifact);
        p.str(3, c.at("edge"));
        p.str(4, a);
        p.str(5, b);
        p.str(6, row["headers"]["protocol"]);
        p.num(7, telemetry::wall());
        p.str(8, body);
        p.next();
      }
      prune();
      recycle();
      runtime::detail::checkpoint("packet.before-commit");
      sql(db, "COMMIT");
      runtime::detail::checkpoint("packet.after-commit");
    } catch (...) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      throw;
    }
  }
  void scan(const Json &run) {
    bool rebuild = run.value("packetRebuild", false);
    std::map<std::string, std::pair<Json, Json>> candidates;
    auto captures = run.value("captureHistory", Json::array());
    for (const auto &c : run.value("captures", Json::array()))
      captures.push_back(c);

    for (const auto &c : captures) {
      {
        std::lock_guard l(mutex);
        if (stop)
          return;
      }
      auto directory = std::filesystem::path(c.at("directory").get<std::string>());
      // Manifests are atomically published; unmanifested/partial files never enter the index.
      Json manifest;
      try {
        manifest = console::load(directory / "manifest.json");
      } catch (...) {
        throw runtime::Failure("capture_manifest_unavailable");
      }
      if (manifest.at("id") != c.at("id") || manifest.at("runId") != run.at("id") ||
          manifest.at("mapping") != c.at("mapping") || manifest.at("bootId") != c.at("bootId"))
        throw runtime::Failure("capture_manifest_identity_changed");
      for (const auto &s : manifest.at("segments")) {

        {
          std::lock_guard l(mutex);
          if (stop)
            return;
        }
        auto seq = std::to_string(integer(s.at("sequence")));
        auto closed = s.at("closedAt").get<std::string>();
        if (!std::regex_match(closed,
                              std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z")))
          throw runtime::Failure("invalid_segment_closed_time");
        auto key = closed + "/" + run.at("id").get<std::string>() + "/" +
                   c.at("id").get<std::string>() + "/" + std::string(19 - seq.size(), '0') + seq;
        candidates.emplace(key, std::make_pair(c, s));
        if (candidates.size() > 1000) {
          candidates.erase(candidates.begin());
          std::lock_guard l(mutex);
          window_truncated = true;
        }
      }
    }
    unsigned accepted = 0;
    for (const auto &[key, pair] : candidates) {
      {
        std::lock_guard l(mutex);
        if (stop)
          return;
      }
      const auto &[c, s] = pair;
      auto seq = std::to_string(integer(s.at("sequence")));
      auto artifact = c.at("id").get<std::string>() + "-" + seq;
      if (known(run.at("id"), artifact, key, rebuild))
        continue;
      if (!rebuild && accepted++ >= 128)
        break;
      auto directory = std::filesystem::path(c.at("directory").get<std::string>());
      Json status = {{"state", "indexed"}, {"sha256", s.at("sha256")}, {"size", s.at("size")}},
           rows = Json::array();
      try {
        if (s.at("state") != "closed" || s.at("file") != seq + ".pcapng")
          throw runtime::Failure("segment_not_finalized");
        auto bytes = verified(directory / (seq + ".pcapng"), s);
        Json context = {{"runId", run.at("id")},
                        {"artifactId", artifact},
                        {"artifactSha256", s.at("sha256")},
                        {"captureId", c.at("id")},
                        {"captureEpoch", c.at("epoch")},
                        {"edge", c.at("edge")},
                        {"interface", c.at("interface")},
                        {"captureEndpoint", c.at("canonicalEndpoint")}};
        auto decoded = decode(bytes, context);
        if (decoded.at("packetCount") != s.at("packets"))
          throw runtime::Failure("manifest_packet_count_mismatch");
        rows = decoded.at("items");
        status["packetCount"] = decoded.at("packetCount");
        status["omittedRecords"] = decoded.at("omittedRecords");
        status["indexedRecords"] = rows.size();
        if (decoded.at("omittedRecords") != "0")
          status["state"] = "record-limit";
      } catch (const std::exception &e) {
        status["state"] = "failed";
        status["error"] = std::string(e.what()).substr(0, 160);
        rows = Json::array();
      }
      save(run, c, artifact, key, status, rows);
    }
  }
  void work() {
    for (;;) {
      Json run;
      {
        std::unique_lock l(mutex);
        condition.wait(l, [&] { return stop || pending.has_value(); });
        if (stop)
          return;
        run = std::move(*pending);
        pending.reset();
        busy = true;
        active_run = run.at("id");
        error.clear();
        retired_in_scan = 0;
        window_truncated = false;
      }
      try {
        if (run.value("packetRebuild", false)) {
          std::lock_guard l(mutex);
          auto m = maintenance();
          m["state"] = "running";
          maintenance(m);
        }
        scan(run);
        if (run.value("packetRebuild", false)) {
          std::lock_guard l(mutex);
          auto m = maintenance();
          m["state"] = stop ? "interrupted" : "completed";
          maintenance(m);
        }
      } catch (const std::exception &e) {
        std::lock_guard l(mutex);
        error = std::string(e.what()).substr(0, 160);
        if (run.value("packetRebuild", false)) {
          try {
            auto m = maintenance();
            m["state"] = "failed";
            m["error"] = error;
            maintenance(m);
          } catch (...) {
          }
        }
      }
      {
        std::lock_guard l(mutex);
        busy = false;
      }
    }
  }
};
History::History(const std::filesystem::path &p) : impl_(std::make_unique<Impl>(p)) {}
History::~History() = default;
Json History::rebuild(const Json &run) {
  std::lock_guard l(impl_->mutex);
  if (impl_->busy || impl_->pending)
    throw runtime::Failure("packet_index_busy", 409);
  Json m = {{"id", console::random_hex(12)},
            {"runId", run.at("id")},
            {"state", "queued"},
            {"operation", "rebuild"}};
  sql(impl_->db, "BEGIN IMMEDIATE");
  try {
    for (auto table : {"packet_rows", "packet_segments"}) {
      Query q(impl_->db, (std::string("DELETE FROM ") + table + " WHERE run=?").c_str());
      q.str(1, run.at("id"));
      q.next();
    }
    sql(impl_->db, "UPDATE packet_meta SET generation=generation+1");
    impl_->maintenance(m);
    runtime::detail::checkpoint("packet.rebuild-before-commit");
    sql(impl_->db, "COMMIT");
  } catch (...) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
  runtime::detail::checkpoint("packet.rebuild-after-commit");
  auto request = run;
  request["packetRebuild"] = true;
  impl_->pending = std::move(request);
  impl_->condition.notify_all();
  return m;
}
void History::recover(const std::filesystem::path &path) {
  auto marker = path.string() + ".recovering", backup = path.string() + ".quarantine",
       replacement = path.string() + ".replacement";
  auto sync_parent = [&] {
    int fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
      throw runtime::Failure("packet_recovery_directory");
    int rc = fsync(fd);
    close(fd);
    if (rc)
      throw runtime::Failure("packet_recovery_sync");
  };
  for (auto suffix : {"", "-journal", "-wal", "-shm", ".recovering", ".quarantine", ".replacement"})
    if (std::filesystem::is_symlink(path.string() + suffix))
      throw runtime::Failure("packet_database_symlink");
  if (!std::filesystem::exists(marker)) {
    for (auto suffix : {"", "-journal", "-wal", "-shm"})
      if (std::filesystem::exists(backup + suffix))
        throw runtime::Failure("packet_quarantine_exists", 409);
    capture::atomic_json(marker, {{"operation", "recover-derived-packet-index"}});
  }
  // With a durable intent, retry is idempotent: quarantine only once, discard incomplete
  // replacement.
  for (auto suffix : {"", "-journal", "-wal", "-shm"}) {
    auto source = path.string() + suffix, target = backup + suffix;
    if (std::filesystem::exists(source)) {
      if (!std::filesystem::exists(target))
        std::filesystem::rename(source, target);
      else
        std::filesystem::remove(source);
    }
    std::filesystem::remove(replacement + suffix);
  }
  sync_parent();
  runtime::detail::checkpoint("packet.recovery-quarantined");
  {
    History fresh(replacement);
  }
  std::filesystem::rename(replacement, path);
  sync_parent();
  runtime::detail::checkpoint("packet.recovery-published");
  std::filesystem::remove(marker);
  sync_parent();
}
Json History::query(const Json &run, const Json &p) {
  if (!p.is_object())
    throw runtime::Failure("invalid_packet_query");
  for (const auto &[k, v] : p.items())
    if (k != "runId" && k != "node" && k != "edge" && k != "protocol" && k != "limit" &&
        k != "cursor")
      throw runtime::Failure("unknown_packet_query_field");
  if (p.contains("limit") &&
      (!p["limit"].is_number_integer() || p["limit"] < 1 || p["limit"] > 200))
    throw runtime::Failure("invalid_packet_limit");
  Json filter = Json::object();
  for (auto key : {"node", "edge", "protocol"}) {
    if (p.contains(key) && (!p[key].is_string() || p[key].get<std::string>().size() > 128))
      throw runtime::Failure("invalid_packet_filter");
    filter[key] = p.value(key, "");
  }
  auto scope = lab_support::digest(Json{{"run", run.at("id")}, {"filter", filter}});
  std::lock_guard l(impl_->mutex);
  sql(impl_->db, "BEGIN IMMEDIATE");
  try {
    impl_->prune();
    sql(impl_->db, "COMMIT");
  } catch (...) {
    sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
  auto generation = scalar(impl_->db, "SELECT generation FROM packet_meta");
  auto top = scalar(impl_->db, "SELECT coalesce(max(id),0) FROM packet_rows"), before = top + 1;
  if (p.contains("cursor")) {
    if (!p["cursor"].is_string() || p["cursor"].get<std::string>().size() > 512)
      throw runtime::Failure("invalid_packet_cursor");
    Json c;
    try {
      c = Json::parse(p["cursor"].get<std::string>());
    } catch (...) {
      throw runtime::Failure("invalid_packet_cursor");
    }
    if (!c.is_object() || !c.contains("generation") || !c.contains("top") ||
        !c.contains("before") || !c.value("scope", Json()).is_string() ||
        !c.value("epoch", Json()).is_string())
      throw runtime::Failure("invalid_packet_cursor");
    if (c.value("scope", "") != scope || c.value("epoch", "") != impl_->epoch)
      throw runtime::Failure("packet_cursor_scope", 409);
    if (c.at("generation") != std::to_string(generation))
      throw runtime::Failure("packet_history_expired", 409);
    top = integer(c.at("top"));
    before = integer(c.at("before"));
  }
  bool scheduled = false;
  if (!impl_->busy && !impl_->pending) {
    impl_->pending = run;
    scheduled = true;
    impl_->condition.notify_all();
  }
  Json items = Json::array();
  auto limit = p.value("limit", 100);
  Query q(impl_->db,
          "SELECT id,body FROM packet_rows WHERE run=? AND id<=? AND id<? AND (?='' OR edge=?) AND "
          "(?='' OR node_a=? OR node_b=?) AND (?='' OR protocol=?) ORDER BY id DESC LIMIT ?");
  q.str(1, run.at("id"));
  q.num(2, top);
  q.num(3, before);
  q.str(4, filter["edge"]);
  q.str(5, filter["edge"]);
  q.str(6, filter["node"]);
  q.str(7, filter["node"]);
  q.str(8, filter["node"]);
  q.str(9, filter["protocol"]);
  q.str(10, filter["protocol"]);
  q.num(11, limit + 1);
  while (q.next()) {
    auto row = Json::parse(q.text(1));
    row["id"] = std::to_string(q.number());
    items.push_back(row);
  }
  Json next = nullptr;
  if (items.size() > std::size_t(limit)) {
    items.erase(items.end() - 1);
    next = Json{{"epoch", impl_->epoch},
                {"scope", scope},
                {"generation", std::to_string(generation)},
                {"top", std::to_string(top)},
                {"before", items.back()["id"]}}
               .dump();
  }
  Json segments = Json::array();
  Query s(impl_->db, "SELECT body FROM packet_segments WHERE run=? ORDER BY artifact");
  s.str(1, run.at("id"));
  while (s.next())
    segments.push_back(Json::parse(s.text()));
  return {
      {"apiVersion", "graphlab.packet-history/v1"},
      {"items", items},
      {"nextCursor", next},
      {"segments", segments},
      {"indexing",
       (impl_->busy && impl_->active_run == run.at("id").get<std::string>()) || scheduled},
      {"indexBusy", impl_->busy},
      {"indexRequestAccepted", scheduled},
      {"indexError", impl_->error.empty() || impl_->active_run != run.at("id").get<std::string>()
                         ? Json(nullptr)
                         : Json(impl_->error)},
      {"retainedRecords", scalar(impl_->db, "SELECT count(*) FROM packet_rows")},
      {"retainedRecordBytes",
       scalar(impl_->db, "SELECT coalesce(sum(length(CAST(body AS BLOB))),0) FROM packet_rows")},
      {"captureCoverage", run.value("captureCoverage", "unavailable")},
      {"retentionSeconds", 86400},
      {"globalRecordLimit", 20000},
      {"globalRecordBytes", 16777216},
      {"databaseMainBytesLimit", 67108864},
      {"segmentRecordLimit", 2000},
      {"segmentByteLimit", max_file},
      {"globalSegmentLimit", 1000},
      {"legacySegmentCount",
       scalar(impl_->db, "SELECT count(*) FROM packet_segments WHERE order_key='' ")},
      {"retiredCatalogEntries", scalar(impl_->db, "SELECT retired FROM packet_retirement")},
      {"retirementBoundary", impl_->floor()},
      {"retiredSegmentsInLastScan",
       impl_->active_run == run.at("id").get<std::string>() ? impl_->retired_in_scan : 0},
      {"scanWindowTruncated", impl_->window_truncated},
      {"maintenance", impl_->maintenance()},
      {"order", "index-id-desc"},
      {"generation", std::to_string(generation)}};
}
} // namespace graphlab::packets
