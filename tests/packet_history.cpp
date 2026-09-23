#include <fstream>
#include <graphlab/capture.hpp>
#include <graphlab/packet_history.hpp>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>
using graphlab::runtime::Json;
using namespace graphlab;
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
  std::cout << "PASS " << m << '\n';
}
template <class F> void rejects(F f, const char *m) {
  bool bad = false;
  try {
    f();
  } catch (...) {
    bad = true;
  }
  check(bad, m);
}
std::vector<unsigned char> read(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
Json wait(packets::History &h, const Json &r, std::size_t count) {
  for (int i = 0; i < 300; i++) {
    auto q = h.query(r, Json::object());
    if (q["segments"].size() >= count)
      return q;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("index timeout");
}
int main() {
  try {
    char temp[] = "/tmp/graphlab-packets-XXXXXX";
    auto root = std::filesystem::path(mkdtemp(temp));
    auto dir = root / "capture";
    std::filesystem::create_directory(dir);
    Json c = {{"id", "cap"},
              {"runId", "run"},
              {"bootId", "boot"},
              {"mapping", Json::object()},
              {"edge", "a-b"},
              {"epoch", "1"},
              {"interface", "data0"},
              {"canonicalEndpoint", "a:data0"},
              {"directory", dir.string()}};
    Json run = {
        {"id", "run"},
        {"captures", Json::array({c})},
        {"captureCoverage", "recording"},
        {"topology",
         {{"edges",
           Json::array({{{"id", "a-b"},
                         {"endpoints", Json::array({"a:data0", "b:data0"})}}})}}}};
    capture::Writer w(
        dir,
        {{"byteBudget", 10000000}, {"snaplen", 65535}, {"interface", "data0"}, {"edge", "a-b"}});
    // Ethernet / IPv4 / UDP, minimal headers, no payload.
    std::vector<unsigned char> packet(42);
    packet[12] = 8;
    packet[14] = 0x45;
    packet[17] = 28;
    packet[23] = 17;
    packet[26] = 10;
    packet[29] = 1;
    packet[30] = 10;
    packet[33] = 2;
    packet[34] = 0x12;
    packet[35] = 0x34;
    packet[36] = 0x23;
    packet[37] = 0x45;
    packet[39] = 8;
    w.open();
    for (int i = 0; i < 3; i++)
      w.packet(1700000000000000ull + i, packet, 42);
    w.close(Json::object());
    Json manifest = c;
    manifest["segments"] = w.segments();
    capture::atomic_json(dir / "manifest.json", manifest);
    auto bytes = read(dir / "0.pcapng");
    auto decoded = packets::decode(bytes, Json::object());
    check(decoded["items"].size() == 3 &&
              decoded["items"][0]["timestampUnixMicros"] == "1700000000000000",
          "EPB timestamp and packet count exact");
    auto row = decoded["items"][0];
    check(row["headers"]["protocol"] == "udp" && row["headers"]["destinationPort"] == 9029 &&
              row["direction"] == "unknown",
          "UDP headers decoded without inferred direction");
    check(packets::decode(bytes, Json::object(), 1)["omittedRecords"] == "2",
          "per-segment record bound reports omissions");
    auto protocol_dir = root / "protocols";
    std::filesystem::create_directory(protocol_dir);
    capture::Writer pw(
        protocol_dir,
        {{"byteBudget", 1000000}, {"snaplen", 65535}, {"interface", "data0"}, {"edge", "a-b"}});
    pw.open();
    auto tcp = packet;
    tcp.resize(54);
    tcp[17] = 40;
    tcp[23] = 6;
    tcp[46] = 0x50;
    pw.packet(1, tcp, 54);
    auto fragment = packet;
    fragment[20] = 0x20;
    pw.packet(2, fragment, 42);
    std::vector<unsigned char> ipv6(62);
    ipv6[12] = 0x86;
    ipv6[13] = 0xdd;
    ipv6[14] = 0x60;
    ipv6[19] = 8;
    ipv6[20] = 17;
    ipv6[55] = 1;
    ipv6[57] = 2;
    ipv6[59] = 8;
    pw.packet(3, ipv6, 62);
    std::vector<unsigned char> short_frame(10);
    pw.packet(4, short_frame, 60);
    auto vlan = packet;
    vlan.insert(vlan.begin() + 12, {0x81, 0x00, 0x00, 0x01});
    pw.packet(5, vlan, 46);
    pw.close(Json::object());
    auto parsed = packets::decode(read(protocol_dir / "0.pcapng"), Json::object())["items"];
    check(parsed[0]["headers"]["protocol"] == "tcp" && parsed[0]["headers"]["sourcePort"] == 4660,
          "TCP headers decoded");
    check(parsed[1]["headers"]["decodeStatus"] == "fragment-no-reassembly" &&
              !parsed[1]["headers"].contains("sourcePort"),
          "fragments never supply guessed ports");
    check(parsed[2]["headers"]["protocol"] == "udp" && parsed[2]["headers"]["destinationPort"] == 2,
          "IPv6 UDP decoded");
    check(parsed[3]["truncated"] == true &&
              parsed[3]["headers"]["decodeStatus"] == "truncated-header",
          "snapshot truncation and short headers explicit");
    check(parsed[4]["headers"]["protocol"] == "udp", "VLAN header offset respected");
    auto bad = bytes;
    bad.pop_back();
    rejects([&] { packets::decode(bad, Json::object()); }, "incomplete block rejected");
    bad = bytes;
    bad[8] = 0;
    rejects([&] { packets::decode(bad, Json::object()); }, "unsupported byte order rejected");
    bad = bytes;
    bad[4] = 255;
    rejects([&] { packets::decode(bad, Json::object()); }, "invalid block length rejected");
    auto dbpath = root / "packets.sqlite";
    std::string cursor;
    {
      packets::History h(dbpath);
      auto q = wait(h, run, 1);
      check(q["items"].size() == 3 && q["segments"][0]["state"] == "indexed",
            "manifested checksum-verified segment indexed");
      auto page = h.query(run, {{"limit", 1}, {"protocol", "udp"}});
      cursor = page["nextCursor"];
      check(page["items"][0]["packetIndex"] == "2", "newest index ordering");
      check(h.query(run, {{"node", "b"}})["items"].size() == 3,
            "node filter uses incident edge endpoints");
      check(h.query(run, {{"edge", "foreign"}})["items"].empty(), "edge filter isolation");
      rejects([&] { h.query(run, {{"limit", 201}}); }, "query limit enforced");
      rejects([&] { h.query(run, {{"cursor", cursor}, {"protocol", "tcp"}}); },
              "cursor bound to filters");
      // A new finalized segment must not appear in the existing pagination snapshot.
      w.open();
      w.packet(1700000000001000ull, packet, 42);
      w.close(Json::object());
      manifest["segments"] = w.segments();
      capture::atomic_json(dir / "manifest.json", manifest);
      q = wait(h, run, 2);
      check(q["items"].size() == 4, "subsequent finalized segment indexed once");
      auto older = h.query(run, {{"limit", 1}, {"protocol", "udp"}, {"cursor", cursor}});
      check(older["items"][0]["packetIndex"] == "1" && older["items"][0]["artifactId"] == "cap-0",
            "stable cursor excludes new segments");
    }
    {
      packets::History h(dbpath);
      auto q = h.query(run, {{"protocol", "udp"}, {"cursor", cursor}});
      check(q["items"].size() == 2, "restart preserves rows and cursor identity");
      Json foreign = run;
      foreign["id"] = "foreign";
      foreign["captures"] = Json::array();
      check(h.query(foreign, Json::object())["items"].empty(), "run isolation");
      rejects([&] { h.query(foreign, {{"protocol", "udp"}, {"cursor", cursor}}); },
              "cross-run cursor denied");
    }
    // Corrupt a new finalized segment; index no data even if its valid prefix looks plausible.
    w.open();
    w.packet(1700000000002000ull, packet, 42);
    w.close(Json::object());
    manifest["segments"] = w.segments();
    capture::atomic_json(dir / "manifest.json", manifest);
    {
      std::fstream f(dir / "2.pcapng", std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(30);
      f.put('x');
    }
    {
      packets::History h(dbpath);
      auto q = wait(h, run, 3);
      check(q["segments"][2]["error"] == "packet_artifact_checksum_mismatch" &&
                q["items"].size() == 4,
            "checksum failure visible without publishing packets");
      w.open();
      w.packet(1700000000003000ull, packet, 42);
      check(h.query(run, Json::object())["segments"].size() == 3, "active partial never indexed");
    }
    sqlite3 *db = nullptr;
    sqlite3_open(dbpath.c_str(), &db);
    sqlite3_exec(db, "UPDATE packet_rows SET time=0", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    {
      packets::History h(dbpath);
      rejects([&] { h.query(run, {{"protocol", "udp"}, {"cursor", cursor}}); },
              "retention invalidates old cursor explicitly");
      check(h.query(run, Json::object())["items"].empty(),
            "expired index rows not rebuilt from retained segments");
    }
    // Exercise both logical ceilings with fixture rows, independently of capture generation.
    sqlite3_open(dbpath.c_str(), &db);
    check(sqlite3_exec(
              db,
              "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<20001) INSERT "
              "INTO packet_rows(run,time,body) SELECT 'capacity',strftime('%s','now'),'{}' FROM n",
              nullptr, nullptr, nullptr) == SQLITE_OK,
          "capacity fixtures inserted");
    sqlite3_close(db);
    {
      packets::History h(dbpath);
      auto q = h.query(run, Json::object());
      check(q["retainedRecords"] == 20000, "global packet record ceiling enforced");
    }
    sqlite3_open(dbpath.c_str(), &db);
    sqlite3_exec(db, "UPDATE packet_rows SET body=printf('%02000d',0)", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    {
      packets::History h(dbpath);
      auto q = h.query(run, Json::object());
      check(q["retainedRecordBytes"].get<std::uint64_t>() <= 16777216 &&
                q["retainedRecords"] < 20000,
            "record byte ceiling enforced independently of count");
    }
    std::filesystem::remove_all(root);
    std::cout << "Packet history tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
