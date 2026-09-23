#include <filesystem>
#include <graphlab/application_telemetry.hpp>
#include <iostream>
#include <lab_support/application_telemetry.hpp>
#include <sqlite3.h>
#include <unistd.h>
using lab_support::Json;
namespace app = lab_support::application_telemetry;
namespace store = graphlab::application_telemetry;
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
  std::cout << "PASS " << why << '\n';
}
template <class F> void rejects(F f, const char *why) {
  bool failed = false;
  try {
    f();
  } catch (...) {
    failed = true;
  }
  check(failed, why);
}
Json report() {
  return {{"apiVersion", "graphlab.application-telemetry/v1"},
          {"stream", "udp-echo"},
          {"epoch", std::string(32, 'a')},
          {"sequence", "1"},
          {"elapsedNs", "1000000000"},
          {"counters",
           {{"sentMessages", "2"},
            {"receivedMessages", "2"},
            {"sentPayloadBytes", "64"},
            {"receivedPayloadBytes", "64"},
            {"errors", "0"},
            {"rejectedMessages", "0"},
            {"backpressureEvents", "0"}}},
          {"latency",
           {{"kind", "local-service-time"},
            {"count", "2"},
            {"sumNs", "30000"},
            {"buckets", Json::array({"1", "1", "0", "0", "0", "0", "0", "0"})}}}};
}
int main() {
  try {
    auto r = report();
    auto first = app::derive(nullptr, r, "collector-a");
    check(first["rates"].is_null(), "first sample has no rate");
    check(first["latency"]["meanUs"] == 15 && first["latency"]["p95UpperBoundUs"] == 50,
          "mean and histogram percentile semantics");
    auto next = r;
    next["sequence"] = "2";
    next["elapsedNs"] = "6000000000";
    next["counters"]["sentMessages"] = "7";
    next["counters"]["sentPayloadBytes"] = "224";
    auto derived = app::derive(first, next, "collector-a");
    check(derived["rates"]["sentMessagesPerSecond"] == 1 &&
              derived["rates"]["sentPayloadBytesPerSecond"] == 32,
          "counter intervals use workload monotonic time");
    check(app::derive(first, next, "collector-b")["gapReason"] == "collector_restarted",
          "collector restart creates a gap");
    auto reset = r;
    reset["epoch"] = std::string(32, 'b');
    check(app::derive(first, reset, "collector-a")["rates"].is_null(),
          "workload restart creates a gap");
    auto gap = next;
    gap["elapsedNs"] = "20000000000";
    check(app::derive(first, gap, "collector-a")["gapReason"] == "sample_gap",
          "long interval does not imply zero traffic");
    auto precise = r;
    precise.erase("latency");
    precise["counters"]["sentPayloadBytes"] = "18446744073709551614";
    auto previous = app::derive(nullptr, precise, "x");
    precise["sequence"] = "2";
    precise["elapsedNs"] = "2000000000";
    precise["counters"]["sentPayloadBytes"] = "18446744073709551615";
    check(app::derive(previous, precise, "x")["rates"]["sentPayloadBytesPerSecond"] == 1,
          "uint64 deltas remain exact before float conversion");
    auto overflow = r;
    overflow["latency"]["buckets"] = Json::array({"0", "0", "0", "0", "0", "0", "0", "2"});
    overflow["latency"]["sumNs"] = "40000000";
    auto hist = app::derive(nullptr, overflow, "x");
    check(hist["latency"]["p95Overflow"] == true && hist["latency"]["p95UpperBoundUs"].is_null(),
          "overflow percentile is never clamped to a false upper bound");
    for (auto key : {"apiVersion", "stream", "epoch", "sequence", "elapsedNs", "counters"}) {
      auto bad = r;
      bad.erase(key);
      rejects([&] { app::validate(bad); }, "missing required report field rejected");
    }
    for (auto value : {"-1", "01", "18446744073709551616", "1.0", ""}) {
      auto bad = r;
      bad["sequence"] = value;
      rejects([&] { app::validate(bad); }, "noncanonical or overflowing integer rejected");
    }
    auto bad = r;
    bad["apiVersion"] = "graphlab.application-telemetry/v2";
    rejects([&] { app::validate(bad); }, "unsupported version rejected");
    bad = r;
    bad["node"] = "foreign";
    rejects([&] { app::validate(bad); }, "workload cannot claim node identity");
    bad = r;
    bad["latency"]["sumNs"] = "99999999";
    rejects([&] { app::validate(bad); }, "inconsistent histogram sum rejected");
    bad = r;
    bad["padding"] = std::string(4000, 'a');
    rejects([&] { app::validate(bad); }, "oversized report rejected");
    rejects([&] { app::derive(first, r, "collector-a"); }, "out of order sequence rejected");
    bad = next;
    bad["counters"]["receivedMessages"] = "1";
    rejects([&] { app::derive(first, bad, "collector-a"); }, "counter reset requires new epoch");
    char path[] = "/tmp/graphlab-app-XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    sqlite3 *db = nullptr;
    sqlite3_open(path, &db);
    store::initialize(db);
    store::ingest(db, "run", "a", r, "collector-a");
    store::ingest(db, "run", "a", r, "collector-a");
    auto query = store::query(db, "run", Json::object(), "collector-a");
    check(query["items"].size() == 1, "duplicate report is not stored twice");
    bad = r;
    bad["counters"]["errors"] = "1";
    rejects([&] { store::ingest(db, "run", "a", bad, "collector-a"); },
            "same sequence with different payload rejected");
    sqlite3_close(db);
    sqlite3_open(path, &db);
    store::initialize(db);
    check(store::query(db, "run", Json::object(), "new")["current"][0]["stale"] == true,
          "persisted samples are stale after collector restart");
    store::ingest(db, "run", "a", next, "new");
    query = store::query(db, "run", {{"limit", 1}}, "new");
    check(query["truncated"] == true && query["items"].size() == 1,
          "bounded history reports truncation");
    check(store::query(db, "foreign", Json::object(), "new")["current"].empty(), "run isolation");
    check(store::query(db, "run", {{"node", "other"}}, "new")["items"].empty(),
          "node filter does not mix streams");
    rejects([&] { store::query(db, "run", {{"limit", 201}}, "new"); }, "oversized query rejected");
    // Fill the table using SQL to test the storage ceiling without thousands of fsyncs.
    sqlite3_exec(
        db,
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10001) INSERT INTO "
        "application_history(run,node,time,body) SELECT 'run','a',strftime('%s','now'),'{}' FROM n",
        nullptr, nullptr, nullptr);
    next["sequence"] = "3";
    next["elapsedNs"] = "11000000000";
    store::ingest(db, "run", "a", next, "new");
    sqlite3_stmt *count = nullptr;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM application_history", -1, &count, nullptr);
    sqlite3_step(count);
    check(sqlite3_column_int(count, 0) == 10000, "global history ceiling enforced");
    sqlite3_finalize(count);
    check(sqlite3_exec(
              db,
              "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1001) INSERT "
              "INTO "
              "application_latest(run,node,time,body) SELECT 'older',cast(x AS TEXT),0,'{}' FROM n;"
              "INSERT INTO application_history(run,node,time,body) VALUES('expired','a',0,'{}')",
              nullptr, nullptr, nullptr) == SQLITE_OK,
          "retention fixtures inserted");
    check(store::query(db, "expired", Json::object(), "new")["items"].empty(),
          "expired history is excluded even before pruning");
    next["sequence"] = "4";
    next["elapsedNs"] = "16000000000";
    store::ingest(db, "run", "a", next, "new");
    sqlite3_prepare_v2(db, "SELECT count(*) FROM application_latest", -1, &count, nullptr);
    sqlite3_step(count);
    check(sqlite3_column_int(count, 0) == 1000, "global latest-record ceiling enforced");
    sqlite3_finalize(count);
    sqlite3_prepare_v2(db, "SELECT count(*) FROM application_history WHERE time=0", -1, &count,
                       nullptr);
    sqlite3_step(count);
    check(sqlite3_column_int(count, 0) == 0, "expired history physically pruned at ingestion");
    sqlite3_finalize(count);
    sqlite3_close(db);
    std::filesystem::remove(path);
    std::cout << "Application telemetry contract/store tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
