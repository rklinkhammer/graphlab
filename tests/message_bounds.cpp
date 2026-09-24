#include <graphlab/capture.hpp>
#include <graphlab/message_history.hpp>
#include <iostream>
#include <lab_support/message_observation.hpp>
#include <sqlite3.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << std::endl;
}
template <class F> void rejects(F f, const char *s) {
  bool bad = false;
  try {
    f();
  } catch (...) {
    bad = true;
  }
  check(bad, s);
}
long scalar(sqlite3 *d, const char *s) {
  sqlite3_stmt *q = nullptr;
  if (sqlite3_prepare_v2(d, s, -1, &q, nullptr) != SQLITE_OK)
    throw std::runtime_error("sql");
  if (sqlite3_step(q) != SQLITE_ROW)
    throw std::runtime_error("row");
  auto n = sqlite3_column_int64(q, 0);
  sqlite3_finalize(q);
  return n;
}
int main() {
  try {
    char path[] = "/tmp/graphlab-message-bounds-XXXXXX";
    std::filesystem::path root = mkdtemp(path);
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};
    lab_support::messages::Reporter reporter(std::string(32, 'a'));
    auto wire = reporter.datagram(0);
    reporter.observe(wire, "send");
    Json c = {{"runId", "run"}, {"node", "a"}, {"instance", "i"}},
         q = {{"runId", "run"}, {"node", "a"}};
    {
      messages::History h(root / "sources.sqlite");
      for (int n = 0; n < 256; n++) {
        c["instance"] = std::to_string(n);
        h.ingest(c, reporter.report());
      }
      check(h.query(q)["sources"].size() == 256 && h.query(q)["sourceCapacityReached"] == true,
            "256 reporter ledgers accepted with visible ceiling");
      c["instance"] = "257";
      rejects([&] { h.ingest(c, reporter.report()); }, "257th source rejected without eviction");
      c["instance"] = "0";
      h.ingest(c, reporter.report());
      check(h.query(q)["sources"].size() == 256, "existing source still works at capacity");
    }
    {
      messages::History h(root / "age.sqlite");
      h.ingest(c, reporter.report(), std::time(nullptr) - 86401);
      check(h.query(q)["items"].empty() && h.query(q)["sources"][0]["expiredOrPruned"] == "1",
            "24-hour expiry omits old events with visible accounting");
      h.ingest(c, reporter.report());
      check(h.query(q)["items"].empty(), "replayed expired sequence cannot resurrect history");
    }
    {
      messages::History h(root / "bytes.sqlite");
      c = {{"runId", std::string(128, 'r')},
           {"node", std::string(128, 'n')},
           {"instance", std::string(128, 'i')}};
      for (int n = 0; n < 520; n++) {
        for (int j = 0; j < 8; j++)
          reporter.observe(reporter.datagram(0), "send");
        auto report = reporter.report();
        for (auto key : {"total", "evicted"})
          report[key] =
              std::to_string(std::stoll(report[key].get<std::string>()) + 100000000000000000LL);
        for (auto &e : report["events"]) {
          e["sequence"] =
              std::to_string(std::stoll(e["sequence"].get<std::string>()) + 100000000000000000LL);
          e["timestampMonotonicNs"] = "100000000000000000";
        }
        h.ingest(c, report);
      }
      sqlite3 *d = nullptr;
      sqlite3_open((root / "bytes.sqlite").c_str(), &d);
      auto count = scalar(d, "SELECT count(*) FROM events"),
           bytes = scalar(d, "SELECT sum(length(body)) FROM events");
      std::cout << "body bytes=" << bytes << " records=" << count << std::endl;
      check(bytes <= 4194304 && bytes > 4190000 && count < 4096,
            "4 MiB body limit prunes before record ceiling for maximum identities");
      sqlite3_close(d);
    }
    auto dir = root / "capture";
    std::filesystem::create_directory(dir);
    Json capture = {{"id", "cap"},
                    {"runId", "run"},
                    {"edge", "edge"},
                    {"epoch", "1"},
                    {"mapping", Json::object()},
                    {"bootId", "boot"},
                    {"directory", dir.string()}};
    Json run = {{"id", "run"},
                {"captures", Json::array({capture})},
                {"captureCoverage", "closed"},
                {"topology", {{"edges", Json::array({{{"id", "edge"}}})}}}};
    Json event = {
        {"id", "1"}, {"runId", "run"}, {"observation", reporter.report()["events"].back()}};
    event["observation"]["messageId"] = wire.substr(4, 48);
    std::vector<unsigned char> packet(95);
    packet[12] = 8;
    packet[14] = 0x45;
    packet[17] = 81;
    packet[23] = 17;
    packet[39] = 61;
    std::copy(wire.begin(), wire.end(), packet.begin() + 42);
    capture::Writer w(
        dir,
        {{"byteBudget", 33554432}, {"snaplen", 65535}, {"interface", "test0"}, {"edge", "edge"}});
    auto publish = [&](Json segments) {
      auto m = capture;
      m["state"] = "closed";
      m["segments"] = segments;
      capture::atomic_json(dir / "manifest.json", m);
      return messages::correlate(run, event, Json::object());
    };
    for (int i = 0; i < 17; i++) {
      w.open();
      w.packet(1, packet, packet.size());
      w.close(Json::object());
    }
    auto r = publish(w.segments());
    check(r["matches"].size() == 16 && r["searchComplete"] == false && r["status"] == "ambiguous",
          "17 segments stop unique-match claims at 16-segment bound");
    w.open();
    for (int i = 0; i < 65; i++)
      w.packet(1, packet, packet.size());
    w.close(Json::object());
    r = publish(Json::array({w.segments().back()}));
    check(r["matches"].size() == 64 && !r["searchComplete"].get<bool>(),
          "65 occurrences retain only 64 with incomplete scan");
    auto unrelated = packet;
    unrelated[42] = 'X';
    w.open();
    for (int i = 0; i < 2000; i++)
      w.packet(1, unrelated, unrelated.size());
    w.packet(1, packet, packet.size());
    w.close(Json::object());
    r = publish(Json::array({w.segments().back()}));
    check(r["status"] == "unavailable" && r["searchComplete"] == false &&
              r["packetsExamined"] == "2000",
          "identifier beyond 2000 decoded packets is unavailable, not inferred");
    auto encrypted = packet;
    encrypted[23] = 50;
    w.open();
    w.packet(1, encrypted, encrypted.size());
    w.close(Json::object());
    r = publish(Json::array({w.segments().back()}));
    check(r["status"] == "unavailable", "ESP packet cannot correlate even with GLM1-looking bytes");
    std::vector<unsigned char> large(65535);
    w.open();
    for (int i = 0; i < 257; i++)
      w.packet(1, large, large.size());
    w.close(Json::object());
    r = publish(Json::array({w.segments().back()}));
    check(r["status"] == "unavailable" && r["searchComplete"] == false && !r["errors"].empty(),
          "segment exceeding 16 MiB scan budget is visibly unavailable");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
