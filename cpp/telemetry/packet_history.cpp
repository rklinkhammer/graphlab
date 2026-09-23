#include <charconv>
#include <fcntl.h>
#include <graphlab/packet_history.hpp>
#include <graphlab/telemetry.hpp>
#include <openssl/evp.h>
#include <regex>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
namespace graphlab::packets {
namespace {
constexpr std::uint64_t max_file = 128ull * 1024 * 1024;
void sql(sqlite3 *db, const char *s) {
  if (sqlite3_exec(db, s, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw runtime::Failure("packet_database_error", 503);
}
struct Query {
  sqlite3_stmt *s = nullptr;
  Query(sqlite3 *db, const char *q) {
    if (sqlite3_prepare_v2(db, q, -1, &s, nullptr) != SQLITE_OK)
      throw runtime::Failure("packet_database_error", 503);
  }
  ~Query() { sqlite3_finalize(s); }
  void str(int i, const std::string &v) {
    sqlite3_bind_text(s, i, v.data(), v.size(), SQLITE_TRANSIENT);
  }
  void num(int i, sqlite3_int64 v) { sqlite3_bind_int64(s, i, v); }
  bool next() {
    auto n = sqlite3_step(s);
    if (n != SQLITE_ROW && n != SQLITE_DONE)
      throw runtime::Failure("packet_database_error", 503);
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
  explicit Impl(const std::filesystem::path &path) {
    if (std::filesystem::is_symlink(path))
      throw runtime::Failure("packet_database_symlink");
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
      throw runtime::Failure("packet_database_open");
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
              "CREATE INDEX IF NOT EXISTS packet_scope ON packet_rows(run,id);");
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
  bool known(const std::string &run, const std::string &artifact) {
    std::lock_guard l(mutex);
    Query q(db, "SELECT 1 FROM packet_segments WHERE run=? AND artifact=?");
    q.str(1, run);
    q.str(2, artifact);
    return q.next();
  }
  void save(const Json &run, const Json &c, const std::string &artifact, Json status, Json rows) {
    std::lock_guard l(mutex);
    if (scalar(db, "SELECT count(*) FROM packet_segments") >= 1000)
      throw runtime::Failure("segment_catalog_capacity");
    sql(db, "BEGIN IMMEDIATE");
    try {
      status["artifactId"] = artifact;
      status["edge"] = c.at("edge");
      Query s(db, "INSERT INTO packet_segments VALUES(?,?,?)");
      s.str(1, run.at("id"));
      s.str(2, artifact);
      s.str(3, status.dump());
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
      sql(db, "COMMIT");
    } catch (...) {
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      throw;
    }
  }
  void scan(const Json &run) {
    auto captures = run.value("captureHistory", Json::array());
    for (const auto &c : run.value("captures", Json::array()))
      captures.push_back(c);
    std::size_t examined = 0;
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
        if (++examined > 1000)
          throw runtime::Failure("segment_scan_capacity");
        {
          std::lock_guard l(mutex);
          if (stop)
            return;
        }
        auto seq = std::to_string(integer(s.at("sequence")));
        auto artifact = c.at("id").get<std::string>() + "-" + seq;
        if (known(run.at("id"), artifact))
          continue;
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
        save(run, c, artifact, status, rows);
      }
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
      }
      try {
        scan(run);
      } catch (const std::exception &e) {
        std::lock_guard l(mutex);
        error = std::string(e.what()).substr(0, 160);
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
      {"order", "index-id-desc"},
      {"generation", std::to_string(generation)}};
}
} // namespace graphlab::packets
