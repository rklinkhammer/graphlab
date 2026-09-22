#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <graphlab/console.hpp>
#include <graphlab/planner.hpp>
#include <iostream>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>
using namespace lab_support;
namespace {
Result<Json> load(const std::string &path) {
  auto text = read_document(path);
  if (!text)
    return std::unexpected(text.error());
  auto parsed = parse_document(*text);
  if (!parsed) {
    auto e = parsed.error();
    e.path = path + ":" + e.path;
    return std::unexpected(e);
  }
  return parsed;
}
int error(const Error &e) {
  std::cerr << Json{{"error", {{"code", e.code}, {"path", e.path}, {"message", e.message}}}}.dump(2)
            << '\n';
  return 2;
}
Json preflight() {
  utsname host{};
  uname(&host);
  Json tools = Json::object();
  for (const auto *tool : {"cmake", "ninja", "c++", "docker", "ovs-vsctl", "ip", "tc",
                           "qemu-system-aarch64", "qemu-system-x86_64"}) {
    tools[tool] = nullptr;
    std::istringstream paths(std::getenv("PATH") ? std::getenv("PATH") : "");
    std::string dir;
    while (std::getline(paths, dir, ':')) {
      if (dir.empty())
        continue;
      auto path = std::filesystem::path(dir) / tool;
      if (access(path.c_str(), X_OK) == 0 && !std::filesystem::is_directory(path)) {
        tools[tool] = path.string();
        break;
      }
    }
  }
  return {
      {"apiVersion", "graphlab.preflight/v1"},
      {"readOnly", true},
      {"version", GRAPHLAB_VERSION},
      {"host", {{"os", host.sysname}, {"architecture", host.machine}, {"kernel", host.release}}},
      {"build",
       {{"compiler", GRAPHLAB_COMPILER},
        {"cppStandard", __cplusplus},
        {"expectedFeature", __cpp_lib_expected},
        {"openssl", crypto_version()}}},
      {"tools", tools},
      {"linuxHost", std::string(host.sysname) == "Linux"},
      {"kvmDevicePresent", std::filesystem::exists("/dev/kvm")},
      {"kvmReadWriteAccess", access("/dev/kvm", R_OK | W_OK) == 0},
      {"runtimeQualified", false},
      {"note", "Paths and device access only. No tool subprocess, daemon request, registry fetch, "
               "or network mutation performed."}};
}
void usage() {
  std::cerr << "Usage:\n  lab preflight\n  lab hash DOCUMENT\n  lab workload CONTRACT\n  lab "
               "validate TOPOLOGY --lock ARTIFACTS\n  lab plan TOPOLOGY --lock ARTIFACTS\n"
               "  lab inspect --socket PATH capabilities|topologies|diagnostics\n"
               "  lab inspect --socket PATH inventory HASH\n"
               "  lab control --socket PATH --agent-uid UID METHOD PARAMS.json\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 8 && std::string(argv[1]) == "control" && std::string(argv[2]) == "--socket" &&
        std::string(argv[4]) == "--agent-uid") {
      uid_t uid;
      std::string input = argv[5];
      auto [end, ec] = std::from_chars(input.data(), input.data() + input.size(), uid);
      if (ec != std::errc{} || end != input.data() + input.size())
        return error({"invalid_uid", "$", "invalid agent UID"});
      auto params = load(argv[7]);
      if (!params)
        return error(params.error());
      std::cout << graphlab::console::rpc(argv[3], uid, argv[6], *params).dump(2) << '\n';
      return 0;
    }
    if ((argc == 5 || argc == 6) && std::string(argv[1]) == "inspect" &&
        std::string(argv[2]) == "--socket") {
      Json params = Json::object();
      if (argc == 6)
        params["hash"] = argv[5];
      std::cout << graphlab::console::rpc(argv[3], geteuid(), argv[4], params).dump(2) << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      usage();
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "preflight") {
      std::cout << preflight().dump(2) << '\n';
      return 0;
    }
    if (argc == 3 && (std::string(argv[1]) == "hash" || std::string(argv[1]) == "workload")) {
      auto doc = load(argv[2]);
      if (!doc)
        return error(doc.error());
      if (std::string(argv[1]) == "hash") {
        std::cout << digest(*doc) << '\n';
        return 0;
      }
      auto checked = validate_workload(*doc);
      if (!checked)
        return error(checked.error());
      std::cout << checked->dump(2) << '\n';
      return 0;
    }
    if (argc != 5 || std::string(argv[3]) != "--lock" ||
        (std::string(argv[1]) != "validate" && std::string(argv[1]) != "plan")) {
      usage();
      return 1;
    }
    auto topology = load(argv[2]);
    if (!topology)
      return error(topology.error());
    auto lock = load(argv[4]);
    if (!lock)
      return error(lock.error());
    if (std::string(argv[1]) == "plan") {
      auto result = graphlab::plan(*topology, *lock);
      if (!result)
        return error(result.error());
      std::cout << result->dump(2) << '\n';
    } else {
      auto result = validate(*topology, *lock);
      if (!result)
        return error(result.error());
      std::cout << Json{{"valid", true},
                        {"topologyHash", result->hash},
                        {"nodes", result->nodes.size()},
                        {"edges", result->edges.size()},
                        {"artifactVerification",
                         "local contract and lock integrity; referenced binaries not fetched"}}
                       .dump(2)
                << '\n';
    }
    return 0;
  } catch (const std::exception &e) {
    return error({"internal_error", "$", e.what()});
  }
}
