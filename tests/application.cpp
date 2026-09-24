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
    Json topology = {
        {"application",
         {{"edges", Json::array({{{"id", "alpha"}, {"source", "b"}, {"target", "a"}},
                                 {{"id", "beta"}, {"source", "b"}, {"target", "a"}}})}}}};
    auto edge = r;
    edge["apiVersion"] = "graphlab.application-edge-telemetry/v1";
    edge["edge"] = "alpha";
    edge["endpoint"] = "target";
    edge["stream"] = "alpha";
    edge["counters"]["sentMessages"] = nullptr;
    edge["counters"]["sentPayloadBytes"] = nullptr;
    edge["counters"]["reconnects"] = "1";
    edge["counters"]["backpressureNs"] = nullptr;
    store::ingest_edge(db, "run", "a", "instance-a", topology, edge, "new");
    store::ingest_edge(db, "run", "a", "instance-a", topology, edge, "new");
    auto beta = edge;
    beta["edge"] = "beta";
    beta["stream"] = "beta";
    beta["counters"]["receivedMessages"] = "9";
    store::ingest_edge(db, "run", "a", "instance-a", topology, beta, "new");
    auto scoped = store::query(db, "run", {{"edge", "alpha"}}, "new");
    check(scoped["current"].size() == 1 && scoped["items"].size() == 1 &&
              scoped["current"][0]["report"]["counters"]["receivedMessages"] == "2",
          "edge scope and dedup preserve independent stream counts");
    check(store::query(db, "run", {{"node", "a"}}, "new")["current"].size() == 1,
          "legacy node queries exclude coexisting edge series");
    check(store::query(db, "run", {{"node", "b"}, {"edge", "alpha"}}, "new")["current"].empty(),
          "node and edge filters intersect");
    rejects([&] { store::ingest_edge(db, "run", "b", "instance-b", topology, edge, "new"); },
            "target report cannot impersonate source");
    rejects([&] { store::ingest_edge(db, "run", "a", "", topology, edge, "new"); },
            "edge report requires agent-owned instance");
    auto wrong = edge;
    wrong["edge"] = "foreign";
    rejects([&] { store::ingest_edge(db, "run", "a", "instance-a", topology, wrong, "new"); },
            "undeclared edge rejected");
    wrong = edge;
    wrong["counters"]["sentMessages"] = "1";
    rejects([&] { app::validate(wrong); }, "target cannot claim source send counters");
    wrong = edge;
    wrong["sequence"] = "2";
    wrong["elapsedNs"] = "2000000000";
    wrong["counters"]["reconnects"] = "0";
    rejects([&] { store::ingest_edge(db, "run", "a", "instance-a", topology, wrong, "new"); },
            "reconnect regression rejected");
    wrong = edge;
    wrong["stream"] = "changed";
    wrong["sequence"] = "2";
    wrong["elapsedNs"] = "2000000000";
    rejects([&] { store::ingest_edge(db, "run", "a", "instance-a", topology, wrong, "new"); },
            "one declared endpoint cannot multiply streams by renaming");
    store::ingest_edge(db, "run", "a", "instance-replaced", topology, edge, "new");
    scoped = store::query(db, "run", {{"edge", "alpha"}}, "new");
    check(scoped["current"][0]["gapReason"] == "workload_instance_changed" &&
              scoped["current"][0]["rates"].is_null(),
          "container replacement cannot carry rate baseline");
    sqlite3_close(db);
    sqlite3_open(path, &db);
    store::initialize(db);
    check(store::query(db, "run", {{"edge", "alpha"}}, "restart")["current"][0]["stale"] == true,
          "edge identity and data survive restart without false freshness");
    auto source = edge;
    source["endpoint"] = "source";
    source.erase("latency");
    source["counters"]["receivedMessages"] = nullptr;
    source["counters"]["receivedPayloadBytes"] = nullptr;
    source["counters"]["sentMessages"] = "9007199254740993";
    source["counters"]["sentPayloadBytes"] = "9007199254740993";
    store::ingest_edge(db, "run", "b", "instance-b", topology, source, "new");
    auto sourceNext = source;
    sourceNext["sequence"] = "2";
    sourceNext["elapsedNs"] = "2000000000";
    sourceNext["counters"]["sentMessages"] = "9007199254740994";
    store::ingest_edge(db, "run", "b", "instance-b", topology, sourceNext, "new");
    auto endpoints = store::query(db, "run", {{"edge", "alpha"}}, "new");
    check(endpoints["current"].size() == 2, "source and target reporters remain separate");
    auto sourceRows = store::query(db, "run", {{"node", "b"}, {"edge", "alpha"}}, "new");
    check(sourceRows["current"][0]["rates"]["sentMessagesPerSecond"] == 1 &&
              sourceRows["current"][0]["rates"]["receivedMessagesPerSecond"].is_null(),
          "edge deltas above JS precision are exact and unowned rates unavailable");
    check(
        sqlite3_exec(db,
                     "CREATE TRIGGER fail_application_history BEFORE INSERT ON application_history "
                     "BEGIN SELECT RAISE(ABORT,'injected storage failure'); END",
                     nullptr, nullptr, nullptr) == SQLITE_OK,
        "storage failure injected");
    sourceNext["sequence"] = "3";
    sourceNext["elapsedNs"] = "3000000000";
    rejects([&] { store::ingest_edge(db, "run", "b", "instance-b", topology, sourceNext, "new"); },
            "failed history insertion rejects edge report");
    check(store::query(db, "run", {{"node", "b"}, {"edge", "alpha"}},
                       "new")["current"][0]["report"]["sequence"] == "2",
          "storage failure rolls back latest baseline atomically");
    sqlite3_exec(db, "DROP TRIGGER fail_application_history", nullptr, nullptr, nullptr);
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
    check(sqlite3_exec(
              db,
              "DELETE FROM application_history; WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT "
              "x+1 FROM n WHERE x<9000) INSERT INTO application_history(run,node,time,body) SELECT "
              "'bytes','a',strftime('%s','now'),json_object('padding',printf('%04000d',0)) FROM n",
              nullptr, nullptr, nullptr) == SQLITE_OK,
          "encoded byte ceiling fixture inserted");
    next["sequence"] = "5";
    next["elapsedNs"] = "17000000000";
    store::ingest(db, "run", "a", next, "new");
    sqlite3_prepare_v2(db, "SELECT sum(length(CAST(body AS BLOB))) FROM application_history", -1,
                       &count, nullptr);
    sqlite3_step(count);
    check(sqlite3_column_int64(count, 0) <= 33554432,
          "encoded history byte ceiling enforced before record limit");
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
