#include <fstream>
#include <graphlab/terminal.hpp>
#include <iostream>
#include <random>
#include <unistd.h>
using namespace graphlab;
void require(bool condition, const char *why) {
  if (!condition)
    throw std::runtime_error(why);
}
int main() {
  try {
    char pattern[] = "/tmp/gl6-terminal-XXXXXX";
    auto *name = mkdtemp(pattern);
    if (!name)
      return 1;
    std::filesystem::path root(name);
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{root};
    std::mt19937 random(0x6c6162);
    std::string expected;
    {
      terminal::Recording recording(root / "stream.partial", 1024 * 1024);
      for (int i = 0; i < 200; ++i) {
        std::string payload(2048, '\0');
        for (auto &b : payload)
          b = char(random());
        expected += payload;
        recording.append(1, payload);
      }
      recording.close();
    }
    // A viewer that waits until after recording closes must still receive exact,
    // bounded pages, without advancing another viewer's independent cursor.
    std::string actual;
    std::uint64_t sequence = 0;
    unsigned pages = 0;
    while (actual.size() < expected.size()) {
      auto result = terminal::replay(root / "stream.glterm", sequence);
      require(!result["partialTail"].get<bool>(), "complete recording marked partial");
      require(result["records"].size() <= 128, "unbounded replay count");
      std::size_t bytes = 0;
      for (const auto &r : result["records"]) {
        auto payload = terminal::decode(r["base64"]);
        require(std::stoull(r["offset"].get<std::string>()) == actual.size(), "replay byte offset");
        bytes += payload.size();
        actual += payload;
      }
      require(bytes > 0 && bytes <= 65536, "unbounded or stalled replay page");
      sequence = std::stoull(result["next"].get<std::string>());
      ++pages;
    }
    require(pages > 1 && actual == expected, "slow viewer byte loss");
    require(terminal::replay(root / "stream.glterm", 0)["records"][0]["sequence"] == "0",
            "viewer cursor shared");
    // Seeded malformed-record corpus: header lengths, types, sequence numbers,
    // arbitrary payload bytes and truncations. Every accepted page stays bounded.
    std::ifstream in(root / "stream.glterm", std::ios::binary);
    std::string seed((std::istreambuf_iterator<char>(in)), {});
    unsigned rejected = 0;
    for (unsigned i = 0; i < 512; ++i) {
      auto bytes = seed.substr(0, 2088);
      if (i % 3 == 0)
        bytes.resize(random() % bytes.size());
      else
        for (unsigned n = 0; n < 8; ++n)
          bytes[random() % std::min<std::size_t>(40, bytes.size())] = char(random());
      std::ofstream(root / "mutated.glterm", std::ios::binary).write(bytes.data(), bytes.size());
      try {
        auto result = terminal::replay(root / "mutated.glterm", 0);
        require(result["records"].size() <= 128, "fuzz replay record bound");
        std::size_t decoded = 0;
        for (const auto &r : result["records"])
          decoded += terminal::decode(r["base64"]).size();
        require(decoded <= 65536, "fuzz replay byte bound");
      } catch (const runtime::Failure &) {
        ++rejected;
      }
    }
    require(rejected > 0, "malformed corpus never rejected");
    std::cout << "PASS delayed independent viewers: " << pages << " bounded pages, "
              << expected.size() << " exact bytes\n";
    std::cout << "PASS 512 seeded malformed recording cases; " << rejected << " rejected\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
