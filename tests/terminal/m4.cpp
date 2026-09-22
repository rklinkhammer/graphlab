#include <fstream>
#include <graphlab/qemu.hpp>
#include <iostream>
#include <unistd.h>
using namespace graphlab;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
  std::cout << "PASS " << s << '\n';
}
int main() {
  try {
    terminal::Lease lease;
    auto now = terminal::Clock::now();
    auto a = lease.acquire("first", false, now);
    bool denied = false;
    try {
      lease.acquire("second", false, now);
    } catch (...) {
      denied = true;
    }
    check(denied, "one writer only");
    auto b = lease.acquire("second", true, now);
    denied = false;
    try {
      lease.require("first", a["token"], now);
    } catch (...) {
      denied = true;
    }
    check(denied, "takeover fences previous writer");
    denied = false;
    try {
      lease.renew("second", b["token"], now + std::chrono::seconds(16));
    } catch (...) {
      denied = true;
    }
    check(denied, "expired lease cannot renew");
    char p[] = "/tmp/m4-record-XXXXXX";
    auto dir = std::filesystem::path(mkdtemp(p));
    struct Cleanup {
      std::filesystem::path p;
      ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{dir};
    {
      terminal::Recording r(dir / "output.partial", 4096);
      r.append(1, std::string_view("a\0b", 3));
      r.append(2, "80x24");
      r.append(3, "input activity only");
      r.sync();
      auto all = terminal::replay(dir / "output.partial", 0);
      check(all["records"].size() == 3 &&
                terminal::decode(all["records"][0]["base64"]) == std::string("a\0b", 3),
            "opaque output replay including NUL");
      auto tail = terminal::replay(dir / "output.partial", 1);
      check(tail["records"][0]["offset"] == "3" && tail["next"] == "3",
            "replay resumes by sequence and output offset");
      r.close();
    }
    check(std::filesystem::exists(dir / "output.glterm") &&
              !std::filesystem::exists(dir / "output.partial"),
          "closed recording published");
    {
      std::ofstream f(dir / "output.glterm", std::ios::app);
      f << "partial";
    }
    check(terminal::replay(dir / "output.glterm", 0)["partialTail"] == true,
          "partial tail visible without modifying bytes");
    std::filesystem::create_symlink(dir / "output.glterm", dir / "link.glterm");
    denied = false;
    try {
      terminal::replay(dir / "link.glterm", 0);
    } catch (...) {
      denied = true;
    }
    check(denied, "replay rejects symlink targets");
    bool quota = false;
    {
      terminal::Recording r(dir / "quota.partial", 48);
      try {
        r.append(1, std::string(40, 'x'));
      } catch (...) {
        quota = true;
      }
    }
    check(quota && std::filesystem::exists(dir / "quota.partial"),
          "quota stops recording without false completion");
    std::cout << "M4 portable tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
