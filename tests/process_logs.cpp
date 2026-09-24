#include <fstream>
#include <graphlab/process_logs.hpp>
#include <graphlab/terminal.hpp>
#include <iostream>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << '\n';
}
template <class F> void rejects(F f, const char *s) {
  bool bad = false;
  try {
    f();
  } catch (const std::exception &) {
    bad = true;
  }
  check(bad, s);
}
Json snapshot(std::string b, std::string generation = "g") {
  return {{"base64", terminal::encode(b)},
          {"generation", generation},
          {"source", "fixture stdout/stderr"},
          {"observedAt", console::timestamp()},
          {"truncated", false},
          {"limitBytes", 65536},
          {"lineLimit", 200}};
}
std::int64_t scalar(const std::filesystem::path &p, const char *q) {
  sqlite3 *d = nullptr;
  sqlite3_open(p.c_str(), &d);
  sqlite3_stmt *s = nullptr;
  sqlite3_prepare_v2(d, q, -1, &s, nullptr);
  sqlite3_step(s);
  auto n = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  sqlite3_close(d);
  return n;
}
void sql(const std::filesystem::path &p, const char *q) {
  sqlite3 *d = nullptr;
  sqlite3_open(p.c_str(), &d);
  auto rc = sqlite3_exec(d, q, nullptr, nullptr, nullptr);
  sqlite3_close(d);
  if (rc)
    throw std::runtime_error("test_sql");
}
int main() {
  try {
    char tmp[] = "/tmp/graphlab-logs-XXXXXX";
    auto dir = std::filesystem::path(mkdtemp(tmp));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir};
    auto path = dir / "logs.sqlite";
    Json c = {{"runId", "run"}, {"node", "a"}, {"source", "docker-output"}},
         q = {{"runId", "run"}, {"node", "a"}};
    std::string id, bytes("\0\xff<script>\n", 11);
    {
      process_logs::History h(path);
      h.ingest(c, snapshot(bytes));
      auto list = h.query(q);
      id = list["items"][0]["id"];
      check(list["items"].size() == 1, "first bounded snapshot atomically published");
      check(terminal::decode(h.download({{"runId", "run"}, {"node", "a"}, {"id", id}})["base64"]) ==
                bytes,
            "invalid UTF-8 and NUL bytes preserved exactly");
      h.ingest(c, snapshot(bytes));
      check(h.query(q)["items"].size() == 1, "unchanged replay coalesced");
      h.ingest(c, snapshot(bytes, "g2"));
      check(h.query(q)["items"][0]["gap"] == "source_generation_changed",
            "source restart gets distinct artifact and gap");
      rejects([&] { h.download({{"runId", "other"}, {"node", "a"}, {"id", id}}); },
              "artifact bound to run");
      rejects([&] { h.download({{"runId", "run"}, {"node", "b"}, {"id", id}}); },
              "artifact bound to node");
      rejects([&] { h.ingest(c, snapshot(std::string(65537, 'x'))); }, "snapshot byte bound");
      for (int i = 0; i < 25; ++i)
        h.ingest(c, snapshot(std::to_string(i)));
      auto page = h.query(q);
      auto cursor = page["nextCursor"];
      auto older = h.query({{"runId", "run"}, {"node", "a"}, {"cursor", cursor}});
      h.ingest(c, snapshot("newest"));
      check(h.query({{"runId", "run"}, {"node", "a"}, {"cursor", cursor}})["items"] ==
                older["items"],
            "older cursor stable while new snapshots arrive");
      rejects([&] { h.query({{"runId", "other"}, {"node", "a"}, {"cursor", cursor}}); },
              "cursor scope isolation");
      sql(path, "CREATE TRIGGER fail_logs BEFORE INSERT ON log_artifacts BEGIN SELECT "
                "RAISE(ABORT,'injected'); END;");
      auto before = h.query(q)["items"];
      rejects([&] { h.ingest(c, snapshot("failed")); }, "storage error visible");
      check(h.query(q)["items"] == before, "failed commit preserves retained artifacts");
      sql(path, "DROP TRIGGER fail_logs;");
      h.ingest(c, snapshot("failed"));
      check(h.query(q)["items"][0]["sha256"] != before[0]["sha256"],
            "retry after exhaustion/failure publishes once");
      sql(path,
          "UPDATE log_artifacts SET payload=x'78' WHERE seq=(SELECT MAX(seq) FROM log_artifacts)");
      auto corrupted = h.query(q)["items"][0]["id"];
      rejects([&] { h.download({{"runId", "run"}, {"node", "a"}, {"id", corrupted}}); },
              "retained download rejects checksum corruption");
    }
    {
      process_logs::History h(path);
      check(h.query(q)["sources"][0]["stale"] == true, "restart marks source observations stale");
      h.ingest(c, snapshot("restart"));
      check(h.query(q)["items"][0]["gap"] == "collector_restarted",
            "restart records attachment gap");
      check(terminal::decode(h.download({{"runId", "run"}, {"node", "a"}, {"id", id}})["base64"]) ==
                bytes,
            "artifact survives collector restart and absent runtime");
      for (int i = 0; i < 80; ++i) {
        auto b = std::string(65536, 'x');
        b.replace(0, std::to_string(i).size(), std::to_string(i));
        h.ingest(c, snapshot(b));
      }
      check(scalar(path, "SELECT SUM(length(payload)) FROM log_artifacts WHERE k=(SELECT k FROM "
                         "log_sources WHERE node='a')") <= 4194304,
            "per-source byte retention bounded");
      check(h.query(q)["sources"][0]["evicted"] != "0", "evictions visible");
      auto expired = c;
      expired["node"] = "old";
      h.ingest(expired, snapshot("expired"), std::time(nullptr) - 90000);
      check(h.query({{"runId", "run"}, {"node", "old"}})["items"].empty(),
            "expired snapshots are pruned and unavailable");
      for (int n = 0; n < 10; ++n) {
        auto x = c;
        x["node"] = "n" + std::to_string(n);
        for (int j = 0; j < 60; ++j)
          h.ingest(x, snapshot(std::string(65500, 'y') + std::to_string(j)));
      }
      check(scalar(path, "SELECT COUNT(*) FROM log_artifacts") <= 512,
            "global artifact count bounded");
      check(scalar(path, "SELECT SUM(length(payload)) FROM log_artifacts") <= 33554432,
            "global artifact byte limit bounded");
      for (int n = 0; n < 244; ++n) {
        auto x = c;
        x["node"] = "empty" + std::to_string(n);
        h.ingest(x, snapshot(""));
      }
      auto overflow = c;
      overflow["node"] = "overflow";
      rejects([&] { h.ingest(overflow, snapshot("")); },
              "source catalog rejects overflow without unbounded tombstones");
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
