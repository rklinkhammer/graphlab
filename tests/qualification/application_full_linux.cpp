#include <fstream>
#include <graphlab/application_telemetry.hpp>
#include <iostream>
#include <sqlite3.h>
#include <sys/statfs.h>
#include <unistd.h>
using namespace graphlab;
using runtime::Json;
int main(int argc, char **argv) {
  try {
    if (argc != 2 || geteuid() != 0)
      return 2;
    std::filesystem::path root = argv[1];
    struct statfs fs{};
    if (statfs(root.c_str(), &fs) || fs.f_type != 0x01021994 ||
        std::uint64_t(fs.f_blocks) * fs.f_bsize > 16 * 1024 * 1024)
      throw std::runtime_error("requires_small_dedicated_tmpfs");
    sqlite3 *d = nullptr;
    if (sqlite3_open((root / "application.sqlite").c_str(), &d) != SQLITE_OK)
      throw std::runtime_error("open");
    struct Close {
      sqlite3 *d;
      ~Close() { sqlite3_close(d); }
    } close{d};
    if (sqlite3_exec(d, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", nullptr, nullptr,
                     nullptr) != SQLITE_OK)
      throw std::runtime_error("pragma");
    application_telemetry::initialize(d);
    Json topology = {
        {"application",
         {{"edges", Json::array({{{"id", "alpha"}, {"source", "b"}, {"target", "a"}}})}}}};
    Json report = {{"apiVersion", "graphlab.application-edge-telemetry/v1"},
                   {"edge", "alpha"},
                   {"endpoint", "target"},
                   {"stream", "alpha"},
                   {"epoch", std::string(32, 'a')},
                   {"sequence", "1"},
                   {"elapsedNs", "1000000000"},
                   {"counters",
                    {{"sentMessages", nullptr},
                     {"sentPayloadBytes", nullptr},
                     {"receivedMessages", "1"},
                     {"receivedPayloadBytes", "32"},
                     {"errors", "0"},
                     {"rejectedMessages", "0"},
                     {"backpressureEvents", "0"},
                     {"reconnects", "0"},
                     {"backpressureNs", nullptr}}}};
    auto ingest = [&] {
      application_telemetry::ingest_edge(d, "run", "a", "instance", topology, report, "collector");
    };
    ingest();
    auto before = application_telemetry::query(d, "run", {{"edge", "alpha"}}, "collector");
    if (sqlite3_wal_checkpoint_v2(d, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr) !=
        SQLITE_OK)
      throw std::runtime_error("checkpoint");
    {
      std::ofstream fill(root / "filler", std::ios::binary);
      std::string b(65536, 'x');
      while (fill)
        fill.write(b.data(), b.size());
    }
    report["sequence"] = "2";
    report["elapsedNs"] = "2000000000";
    report["counters"]["receivedMessages"] = "2";
    report["counters"]["receivedPayloadBytes"] = "64";
    bool failed = false;
    try {
      ingest();
    } catch (const std::exception &e) {
      failed = true;
      std::cout << e.what() << std::endl;
    }
    auto after = application_telemetry::query(d, "run", {{"edge", "alpha"}}, "collector");
    if (!failed || after["items"] != before["items"] || after["current"] != before["current"])
      throw std::runtime_error("full_partial_publication");
    std::cout << "PASS application edge ENOSPC preserves current and history atomically"
              << std::endl;
    std::filesystem::remove(root / "filler");
    ingest();
    if (application_telemetry::query(d, "run", {{"edge", "alpha"}}, "collector")["items"].size() !=
        2)
      throw std::runtime_error("retry");
    std::cout << "PASS application edge retry after freeing space publishes full sample"
              << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
