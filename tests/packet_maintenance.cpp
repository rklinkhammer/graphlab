#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <graphlab/packet_history.hpp>
#include <iostream>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
using namespace graphlab;
using runtime::Json;
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
  std::cout << "PASS " << m << std::endl;
}
long long count(const std::filesystem::path &p, const char *sql) {
  sqlite3 *db = nullptr;
  sqlite3_open(p.c_str(), &db);
  sqlite3_stmt *s = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  sqlite3_step(s);
  auto n = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  sqlite3_close(db);
  return n;
}
void execute(const std::filesystem::path &p, const char *sql) {
  sqlite3 *db = nullptr;
  sqlite3_open(p.c_str(), &db);
  auto rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
  sqlite3_close(db);
  check(rc == SQLITE_OK, "fixture SQL succeeds");
}
struct Fixture {
  std::filesystem::path root, dir;
  Json run, c, segment;
  Fixture(std::filesystem::path p, int packets = 1) : root(p), dir(p / "capture") {
    std::filesystem::create_directories(dir);
    c = {{"id", "capture"},          {"runId", "run"},
         {"bootId", "boot"},         {"mapping", Json::object()},
         {"edge", "edge"},           {"epoch", "1"},
         {"interface", "data0"},     {"canonicalEndpoint", "a:data0"},
         {"directory", dir.string()}};
    run = {{"id", "run"},
           {"captures", Json::array({c})},
           {"topology",
            {{"edges", Json::array({{{"id", "edge"},
                                     {"endpoints", Json::array({"a:data0", "b:data0"})}}})}}}};
    capture::Writer writer(
        dir,
        {{"byteBudget", 10000000}, {"snaplen", 65535}, {"interface", "data0"}, {"edge", "edge"}});
    writer.open();
    std::vector<unsigned char> bytes(60);
    for (int i = 0; i < packets; i++)
      writer.packet(1700000000000000ull + i, bytes, 60);
    writer.close(Json::object());
    segment = writer.segments()[0];
    manifest(1);
    capture::atomic_json(root / "run.json", run);
  }
  void manifest(int n) {
    auto m = c;
    m["segments"] = Json::array();
    for (int i = 0; i < n; i++) {
      auto s = segment;
      s["sequence"] = std::to_string(i);
      s["file"] = std::to_string(i) + ".pcapng";
      auto dest = dir / s["file"].get<std::string>();
      if (i && !std::filesystem::exists(dest))
        std::filesystem::create_hard_link(dir / "0.pcapng", dest);
      m["segments"].push_back(s);
    }
    capture::atomic_json(dir / "manifest.json", m);
  }
};
template <class F> Json until(packets::History &h, const Json &r, F done) {
  for (int i = 0; i < 2000; i++) {
    auto q = h.query(r, Json::object());
    if (done(q))
      return q;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("maintenance timeout");
}
void child(const std::string &self, const std::filesystem::path &root,
           const std::filesystem::path &db, const std::string &point, const std::string &action) {
  auto pid = fork();
  if (pid == 0) {
    setenv("GRAPHLAB_CRASH_AT", point.c_str(), 1);
    execl(self.c_str(), self.c_str(), "--child", root.c_str(), db.c_str(), action.c_str(), nullptr);
    _exit(99);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "process killed at requested durable boundary");
}
int main(int argc, char **argv) {
  try {
#ifdef __linux__
    if (argc == 3 && std::string(argv[1]) == "--filesystem-full") {
      std::filesystem::path root = argv[2];
      struct statfs fs{};
      check(root.string().find("/gl-packet-full-") != std::string::npos &&
                !statfs(root.c_str(), &fs) && fs.f_type == 0x01021994 &&
                fs.f_blocks * fs.f_bsize <= 16 * 1024 * 1024,
            "disk-full test restricted to dedicated small tmpfs");
      Fixture f(root / "fixture", 2000);
      auto db = f.root / "index.sqlite";
      {
        packets::History h(db);
      }
      auto filler = root / "filler";
      int fd = open(filler.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
      check(fd >= 0, "create isolated filler");
      char bytes[65536]{};
      std::uint64_t size = 0;
      for (;;) {
        auto n = write(fd, bytes, sizeof(bytes));
        if (n > 0) {
          size += n;
          continue;
        }
        check(n < 0 && errno == ENOSPC, "tmpfs reaches real ENOSPC");
        break;
      }
      check(size > 131072 && ftruncate(fd, size - 131072) == 0,
            "reserve bounded room for SQLite transaction startup");
      close(fd);
      {
        packets::History h(db);
        auto q =
            until(h, f.run, [](const Json &q) { return q["indexError"] == "packet_storage_full"; });
        check(q["retainedRecords"] == 0 && q["segments"].empty(),
              "filesystem exhaustion rolls back packet rows and receipt");
      }
      std::filesystem::remove(filler);
      {
        packets::History h(db);
        until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 2000; });
      }
      check(count(db, "SELECT count(*) FROM packet_rows") == 2000,
            "freeing filesystem space permits exactly one retry");
      std::filesystem::remove_all(f.root);
      return 0;
    }
#endif
    if (argc == 5 && std::string(argv[1]) == "--child") {
      auto run = console::load(std::filesystem::path(argv[2]) / "run.json");
      std::string action = argv[4];
      if (action == "recover") {
        packets::History::recover(argv[3]);
        return 2;
      }
      packets::History h(argv[3]);
      if (action == "rebuild")
        h.rebuild(run);
      for (int i = 0; i < 500; i++) {
        h.query(run, Json::object());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return 3;
    }
    char path[] = "/tmp/graphlab-packet-maintenance-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(path));
    Fixture f(root);
    auto db = root / "history.sqlite";
    f.manifest(1000);
    {
      packets::History h(db);
      until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 1000; });
    }
    f.manifest(1020);
    std::string cursor;
    {
      packets::History h(db);
      auto q = until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 1020; });
      check(q["segments"].size() == 1000 && q["retiredCatalogEntries"] == 20,
            "catalog recycles beyond lifetime ceiling");
      check(q["scanWindowTruncated"] == true, "bounded newest-segment window is explicit");
      cursor = h.query(f.run, {{"limit", 1}})["nextCursor"];
    }
    // Make retired segments visible again in a smaller manifest, including across restart.
    f.manifest(1000);
    {
      packets::History h(db);
      auto q = until(h, f.run,
                     [](const Json &q) { return q["retiredSegmentsInLastScan"].get<int>() >= 20; });
      check(q["retainedRecords"] == 1020, "evicted receipts cannot replay packets after restart");
    }
    // Explicit rebuild replays retired source files, with no duplicates and a new cursor
    // generation.
    f.manifest(3);
    {
      packets::History h(db);
      auto m = h.rebuild(f.run);
      check(m["state"] == "queued", "rebuild admitted");
      auto q = until(h, f.run, [](const Json &q) {
        return q["maintenance"].is_object() && q["maintenance"]["state"] == "completed";
      });
      check(q["retainedRecords"] == 3, "rebuild replaces scoped rows including retired artifacts");
      bool rejected = false;
      try {
        h.query(f.run, {{"cursor", cursor}});
      } catch (const runtime::Failure &e) {
        rejected = e.status == 409;
      }
      check(rejected, "rebuild invalidates old pagination");
    }
    auto sha = capture::file_hash(f.dir / "0.pcapng");
    {
      std::ofstream corrupt(db, std::ios::binary | std::ios::trunc);
      corrupt << "not SQLite";
    }
    bool bad = false;
    try {
      packets::History h(db);
    } catch (...) {
      bad = true;
    }
    check(bad, "corrupt index fails closed");
    packets::History::recover(db);
    check(std::filesystem::exists(db.string() + ".quarantine"),
          "recovery preserves bounded quarantine copy");
    {
      packets::History h(db);
      auto q = until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 3; });
      check(q["segments"].size() == 3, "recovery indexes verified source again");
      bool rejected = false;
      try {
        h.query(f.run, {{"cursor", cursor}});
      } catch (const runtime::Failure &e) {
        rejected = e.status == 409;
      }
      check(rejected, "recovery changes database epoch and rejects prior cursors");
    }
    check(capture::file_hash(f.dir / "0.pcapng") == sha,
          "maintenance preserves source artifact bytes");
    bad = false;
    try {
      packets::History::recover(db);
    } catch (const runtime::Failure &e) {
      bad = e.status == 409;
    }
    check(bad, "existing quarantine is not overwritten");
    // Migrate bounded legacy receipts without replay or dropping their rows.
    execute(db, "UPDATE packet_segments SET order_key=''");
    {
      packets::History h(db);
      until(h, f.run, [](const Json &q) { return q["legacySegmentCount"] == 0; });
    }
    check(count(db, "SELECT count(*) FROM packet_rows") == 3,
          "legacy receipts acquire retirement keys without replay");
#ifdef GRAPHLAB_TEST_CHECKPOINTS
    for (auto point : {"packet.before-commit", "packet.after-commit"}) {
      auto dest = root / (std::string(point) + ".sqlite");
      child(argv[0], root, dest, point, "scan");
      auto expected = std::string(point) == "packet.before-commit" ? 0 : 1;
      check(count(dest, "SELECT count(*) FROM packet_rows") == expected,
            "killed transaction publishes all rows or none");
      {
        packets::History h(dest);
        until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 3; });
      }
      check(count(dest, "SELECT count(*) FROM packet_rows") == 3,
            "restart retries without duplicate packets");
    }
    for (auto point : {"packet.rebuild-before-commit", "packet.rebuild-after-commit"}) {
      auto dest = root / (std::string(point) + ".sqlite");
      {
        packets::History h(dest);
        until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 3; });
      }
      child(argv[0], root, dest, point, "rebuild");
      check(count(dest, "SELECT count(*) FROM packet_rows") ==
                (std::string(point) == "packet.rebuild-before-commit" ? 3 : 0),
            "rebuild clearing is atomic");
      {
        packets::History h(dest);
        if (std::string(point) == "packet.rebuild-after-commit")
          check(count(dest, "SELECT count(*) FROM packet_maintenance WHERE body LIKE "
                            "'%\"state\":\"interrupted\"%'") == 1,
                "restart marks committed but unfinished rebuild interrupted");
        h.rebuild(f.run);
        until(h, f.run, [](const Json &q) { return q["maintenance"]["state"] == "completed"; });
      }
    }
    for (auto point : {"packet.recovery-quarantined", "packet.recovery-published"}) {
      auto dest = root / (std::string(point) + ".sqlite");
      {
        packets::History h(dest);
      }
      child(argv[0], root, dest, point, "recover");
      bad = false;
      try {
        packets::History h(dest);
      } catch (...) {
        bad = true;
      }
      check(bad, "interrupted recovery blocks automatic opening");
      packets::History::recover(dest);
      {
        packets::History h(dest);
        until(h, f.run, [](const Json &q) { return q["retainedRecords"] == 3; });
      }
    }
    Fixture full(root / "full", 2000);
    auto full_db = full.root / "index.sqlite";
    setenv("GRAPHLAB_PACKET_TEST_PAGES", "16", 1);
    {
      packets::History h(full_db);
      auto q = until(h, full.run,
                     [](const Json &q) { return q["indexError"] == "packet_storage_full"; });
      check(q["retainedRecords"] == 0 && q["segments"].empty(),
            "SQLITE_FULL rolls back receipts and rows together");
    }
    unsetenv("GRAPHLAB_PACKET_TEST_PAGES");
    {
      packets::History h(full_db);
      until(h, full.run, [](const Json &q) { return q["retainedRecords"] == 2000; });
    }
    check(count(full_db, "SELECT count(*) FROM packet_rows") == 2000,
          "storage capacity restored: retry succeeds once");
#endif
    std::filesystem::remove_all(root);
    std::cout << "Packet maintenance tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
