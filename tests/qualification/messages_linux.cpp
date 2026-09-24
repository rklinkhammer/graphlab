#include <fstream>
#include <graphlab/message_history.hpp>
#include <iostream>
#include <lab_support/message_observation.hpp>
#include <sys/statfs.h>
#include <unistd.h>
using namespace graphlab;
int main(int argc, char **argv) {
  try {
    if (argc != 2 || geteuid() != 0)
      return 2;
    std::filesystem::path root = argv[1];
    struct statfs fs{};
    if (statfs(root.c_str(), &fs) || fs.f_type != 0x01021994 ||
        std::uint64_t(fs.f_blocks) * fs.f_bsize > 16 * 1024 * 1024)
      throw std::runtime_error("requires_small_dedicated_tmpfs");
    messages::History history(root / "messages.sqlite");
    lab_support::messages::Reporter reporter(std::string(32, 'a'));
    reporter.observe(reporter.datagram(0), "send");
    runtime::Json c = {{"runId", "full"}, {"node", "a"}, {"instance", "fixture"}},
                  q = {{"runId", "full"}, {"node", "a"}};
    history.ingest(c, reporter.report());
    auto before = history.query(q)["items"];
    {
      std::ofstream fill(root / "filler", std::ios::binary);
      std::string bytes(65536, 'x');
      while (fill)
        fill.write(bytes.data(), bytes.size());
    }
    for (int n = 0; n < 8; ++n)
      reporter.observe(reporter.datagram(0), "send");
    bool failed = false;
    try {
      history.ingest(c, reporter.report());
    } catch (const std::exception &e) {
      std::cout << e.what() << std::endl;
      failed = true;
    }
    if (!failed || history.query(q)["items"] != before)
      throw std::runtime_error("ENOSPC_atomicity_failed");
    std::cout << "PASS actual ENOSPC preserves committed events and rejects new report"
              << std::endl;
    std::filesystem::remove(root / "filler");
    history.ingest(c, reporter.report());
    if (history.query(q)["items"].size() != 9)
      throw std::runtime_error("retry_failed");
    std::cout << "PASS retry after freeing space publishes complete batch" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
