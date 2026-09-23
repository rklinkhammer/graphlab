// Scope cleanup to explicit retained fixture directories, never a unit wildcard.
#include <fcntl.h>
#include <graphlab/terminal.hpp>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab;
int main(int argc, char **argv) {
  if (argc < 2 || geteuid())
    return 2;
  int lock = open("/run/graphlab-executor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
    return 2;
  try {
    unsigned cleaned = 0;
    for (int i = 1; i < argc; i++)
      for (const auto &entry : std::filesystem::recursive_directory_iterator(argv[i])) {
        if (entry.path().filename() != "config.json")
          continue;
        struct stat st{};
        if (lstat(entry.path().c_str(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
            (st.st_mode & 0022))
          throw std::runtime_error("unsafe fixture config");
        auto d = console::load(entry.path());
        if (!d.contains("unit"))
          continue;
        if (std::filesystem::path(d.at("directory").get<std::string>()) !=
            entry.path().parent_path())
          throw std::runtime_error("fixture directory identity mismatch");
        auto state = runtime::process(
            {"/usr/bin/systemctl", "show", d["unit"], "--property=ActiveState", "--value"});
        if (state.output != "inactive\n" && state.output != "failed\n")
          throw std::runtime_error("fixture worker is not stopped");
        auto generation = d.value("generation", std::string("1"));
        if (d.contains("kind"))
          terminal::stop(d, generation);
        else
          capture::control(
              {{"captures", lab_support::Json::array({d})}, {"controllerGeneration", generation}},
              "stop");
        auto remaining = runtime::process(
            {"/usr/bin/systemctl", "list-units", "--all", "--no-legend", d["unit"]});
        if (remaining.code || !remaining.output.empty())
          throw std::runtime_error("fixture unit remains");
        for (auto name : {"control.sock", "attach.sock", "serial.sock", "qmp.sock"})
          if (std::filesystem::exists(entry.path().parent_path() / name))
            throw std::runtime_error("fixture socket remains");
        std::cout << "PASS collected stopped worker " << d["unit"] << " from " << entry.path()
                  << std::endl;
        ++cleaned;
      }
    std::cout << "PASS scoped stopped-worker cleanup " << cleaned << " descriptors\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
