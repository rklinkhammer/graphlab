#include <fstream>
#include <graphlab/capture.hpp>
#include <graphlab/message_history.hpp>
#include <iostream>
#include <lab_support/message_observation.hpp>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool v, const char *s) {
  if (!v)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
template <class F> void rejects(F f, const char *s) {
  bool fail = false;
  try {
    f();
  } catch (...) {
    fail = true;
  }
  check(fail, s);
}
int main() {
  try {
    char tmp[] = "/tmp/graphlab-messages-XXXXXX";
    std::filesystem::path root = mkdtemp(tmp);
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};
    lab_support::messages::Reporter reporter(std::string(32, 'a'));
    auto wire = reporter.datagram(0);
    reporter.observe(wire, "send");
    auto report = reporter.report();
    check(report["events"].size() == 1 && !report.contains("payload"),
          "fixture emits explicit metadata without payload storage");
    messages::validate(report);
    auto bad = report;
    bad["events"][0]["payload"] = "secret";
    rejects([&] { messages::validate(bad); }, "payload recording rejected");
    bad = report;
    bad["events"][0]["sequence"] = "2";
    rejects([&] { messages::validate(bad); }, "noncontiguous report rejected");
    Json context = {{"runId", "run"}, {"node", "a"}, {"instance", "container"}},
         query = {{"runId", "run"}, {"node", "a"}};
    std::string first;
    {
      messages::History store(root / "messages.sqlite");
      store.ingest(context, report);
      store.ingest(context, report);
      check(store.query(query)["items"].size() == 1, "duplicate report replay coalesces");
      first = store.query(query)["items"][0]["id"];
      auto conflict = report;
      conflict["events"][0]["kind"] = "receive";
      rejects([&] { store.ingest(context, conflict); }, "event sequence mutation rejected");
      for (int i = 0; i < 28; ++i) {
        reporter.observe(reporter.datagram(0), "send");
        store.ingest(context, reporter.report());
      }
      auto page = store.query(query), cursor = page["nextCursor"],
           older = store.query({{"runId", "run"}, {"node", "a"}, {"cursor", cursor}});
      reporter.observe(wire, "receive");
      store.ingest(context, reporter.report());
      check(store.query({{"runId", "run"}, {"node", "a"}, {"cursor", cursor}})["items"] ==
                older["items"],
            "older pages stable across inserts");
      rejects([&] { store.query({{"runId", "wrong"}, {"node", "a"}, {"cursor", cursor}}); },
              "cross-run cursor rejected");
      rejects([&] { store.event({{"runId", "wrong"}, {"node", "a"}, {"id", first}}); },
              "cross-run event lookup rejected");
      for (int i = 0; i < 20; ++i)
        reporter.observe(wire, "receive");
      store.ingest(context, reporter.report());
      check(store.query(query)["sources"][0]["missedBeforeIngestion"] != "0",
            "overwritten report observations visible as omissions, not delivery loss");
    }
    messages::History store(root / "messages.sqlite");
    check(store.query(query)["sources"][0]["stale"] == true,
          "restart marks old observations stale");
    auto event = store.event({{"runId", "run"}, {"node", "a"}, {"id", first}});
    check(event["observation"]["messageId"] == lab_support::messages::identifier(wire)["messageId"],
          "event identity survives restart");
    auto dir = root / "capture";
    std::filesystem::create_directory(dir);
    Json c = {{"id", "capture"},
              {"runId", "run"},
              {"edge", "a-b"},
              {"epoch", "1"},
              {"bootId", "boot"},
              {"mapping", Json::object()},
              {"directory", dir.string()}};
    Json run = {{"id", "run"},
                {"captures", Json::array({c})},
                {"captureCoverage", "closed"},
                {"topology", {{"edges", Json::array({{{"id", "a-b"}}})}}}};
    std::vector<unsigned char> packet(14 + 20 + 8 + wire.size());
    packet[12] = 8;
    packet[14] = 0x45;
    packet[17] = 20 + 8 + wire.size();
    packet[23] = 17;
    packet[36] = 0xbf;
    packet[37] = 0x68;
    packet[39] = 8 + wire.size();
    std::copy(wire.begin(), wire.end(), packet.begin() + 42);
    capture::Writer writer(
        dir,
        {{"byteBudget", 1048576}, {"snaplen", 65535}, {"interface", "test0"}, {"edge", "a-b"}});
    writer.open();
    writer.packet(1700000000000000, packet, packet.size());
    writer.close(Json::object());
    auto manifest = c;
    manifest["state"] = "closed";
    manifest["segments"] = writer.segments();
    capture::atomic_json(dir / "manifest.json", manifest);
    auto correlation = messages::correlate(run, event, Json::object());
    check(correlation["status"] == "exact" && correlation["matches"].size() == 1 &&
              correlation["matches"][0]["packetIndex"] == "0",
          "one verified wire identity gives precise packet reference");
    writer.open();
    writer.packet(1700000000000001, packet, packet.size());
    writer.close(Json::object());
    manifest["segments"] = writer.segments();
    capture::atomic_json(dir / "manifest.json", manifest);
    check(messages::correlate(run, event, Json::object())["status"] == "ambiguous",
          "duplicate/reused wire ID occurrences remain ambiguous");
    auto unrelated = event;
    unrelated["observation"]["messageId"] = std::string(48, 'b');
    check(messages::correlate(run, unrelated, Json::object())["status"] == "unavailable",
          "unrelated traffic is not correlated by tuple or timestamp");
    writer.open();
    writer.packet(1700000000000002,
                  std::span<const unsigned char>(packet.data(), packet.size() - 1), packet.size());
    writer.close(Json::object());
    manifest["segments"] = Json::array({writer.segments()[2]});
    capture::atomic_json(dir / "manifest.json", manifest);
    auto truncated = messages::correlate(run, event, Json::object());
    check(truncated["status"] == "unavailable" && truncated["searchComplete"] == false,
          "truncated datagram cannot produce exact correlation");
    auto wrong = run;
    wrong["id"] = "other";
    rejects([&] { messages::correlate(wrong, event, Json::object()); },
            "wrong-run correlation rejected");
    manifest["segments"] = Json::array({writer.segments()[0]});
    manifest["state"] = "recording";
    capture::atomic_json(dir / "manifest.json", manifest);
    check(messages::correlate(run, event, Json::object())["status"] == "ambiguous",
          "active capture cannot establish unique correlation");
    manifest["state"] = "closed";
    manifest["segments"][0]["sha256"] = "sha256:bad";
    capture::atomic_json(dir / "manifest.json", manifest);
    auto corrupt = messages::correlate(run, event, Json::object());
    check(corrupt["status"] == "unavailable" && !corrupt["errors"].empty(),
          "checksum failures expose no matches");
    sqlite3 *faultdb = nullptr;
    sqlite3_open((root / "messages.sqlite").c_str(), &faultdb);
    sqlite3_exec(faultdb,
                 "CREATE TRIGGER fail_events BEFORE INSERT ON events BEGIN SELECT "
                 "RAISE(ABORT,'injected'); END;",
                 nullptr, nullptr, nullptr);
    auto beforeFailure = store.query(query)["items"];
    reporter.observe(reporter.datagram(0), "send");
    rejects([&] { store.ingest(context, reporter.report()); },
            "insertion failure rejected atomically");
    check(store.query(query)["items"] == beforeFailure, "failed transaction preserves old events");
    sqlite3_exec(faultdb, "DROP TRIGGER fail_events", nullptr, nullptr, nullptr);
    sqlite3_close(faultdb);
    store.ingest(context, reporter.report());
    for (int n = 0; n < 600; ++n) {
      for (int j = 0; j < 8; ++j)
        reporter.observe(reporter.datagram(j % 2), "send");
      store.ingest(context, reporter.report());
    }
    auto bounded = store.query(query);
    check(std::stoll(bounded["sources"][0]["retained"].get<std::string>()) <= 4096 &&
              bounded["sources"][0]["expiredOrPruned"] != "0",
          "store record and byte pruning visible");
    auto epoch = reporter.report();
    epoch["epoch"] = std::string(32, 'c');
    store.ingest(context, epoch);
    check(store.query(query)["sources"].size() == 2, "reporter epochs remain distinct");
    auto beforeCrash = store.query(query)["items"];
    auto child = fork();
    if (child == 0) {
      sqlite3 *d = nullptr;
      sqlite3_open((root / "messages.sqlite").c_str(), &d);
      if (sqlite3_exec(d, "BEGIN IMMEDIATE; DELETE FROM events;", nullptr, nullptr, nullptr) !=
          SQLITE_OK)
        _exit(2);
      kill(getpid(), SIGKILL);
      _exit(3);
    }
    int childStatus = 0;
    if (child < 0 || waitpid(child, &childStatus, 0) != child)
      throw std::runtime_error("crash_fixture_failed");
    check(WIFSIGNALED(childStatus) && WTERMSIG(childStatus) == SIGKILL &&
              store.query(query)["items"] == beforeCrash,
          "SQLite hot-journal recovery preserves committed events after SIGKILL mid-transaction");
    struct Slow : runtime::Backend {
      std::atomic<bool> entered = false, release = false;
      Json message_report(const Json &, const Json &) override {
        entered = true;
        while (!release)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return nullptr;
      }
      void preflight(const Json &, const Json &) override {}
      Json prepare(const Json &, const Json &) override { return nullptr; }
      void remove(const Json &, const Json &) override {}
      void activate(const Json &) override {}
      void gate(const Json &, const std::string &) override {}
      Json observe(const Json &) override { return nullptr; }
    } slow;
    {
      messages::History isolated(root / "slow.sqlite");
      isolated.start(slow, [] {
        return Json::array({{{"run", {{"id", "run"}}},
                             {"resource", {{"logical", "a"}, {"identity", {{"id", "i"}}}}}}});
      });
      for (int i = 0; i < 700 && !slow.entered; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      auto start = std::chrono::steady_clock::now();
      auto empty = isolated.query(query);
      auto elapsed = std::chrono::steady_clock::now() - start;
      slow.release = true;
      check(slow.entered && elapsed < std::chrono::seconds(1) && empty["items"].empty(),
            "slow runtime source does not hold the history read mutex");
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
