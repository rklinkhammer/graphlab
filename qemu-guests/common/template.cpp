#include <fstream>
#include <graphlab/capture.hpp>
#include <iostream>
using graphlab::runtime::Json;
int main(int argc, char **argv) {
  try {
    if (argc != 4)
      throw std::runtime_error(
          "usage: lab-guest-template PROFILE ARTIFACT_DIRECTORY OUTPUT_DIRECTORY");
    std::string profile = argv[1];
    if (profile != "ppc64le-tcg" && profile != "arm64-kvm")
      throw std::runtime_error("unsupported profile");
    auto input = std::filesystem::path(argv[2]), out = std::filesystem::path(argv[3]);
    std::filesystem::create_directories(out);
    auto platform = profile == "ppc64le-tcg" ? "linux/ppc64le" : "linux/arm64";
    Json interfaces = Json::object();
    for (auto port : {"data0", "mgmt0"})
      interfaces[port] = {{"medium", "ethernet"},
                          {"mtuRange", Json::array({1280, 9000})},
                          {"required", true},
                          {"role", std::string(port) == "data0" ? "data" : "management"}};
    auto hash = [&](std::string name) { return graphlab::capture::file_hash(input / name); };
    Json contract = {
        {"apiVersion", "graphlab.workload/v2"},
        {"id", "guest-linux"},
        {"kind", "qemu"},
        {"platforms", Json::array({platform})},
        {"interfaces", interfaces},
        {"lifecycle", {{"gateUntilRelease", true}, {"quiesce", "supported"}}},
        {"resources", {{"cpus", 1}, {"memoryMiB", 512}}},
        {"labSupport",
         {{"language", "cpp23"}, {"version", "1.0.0"}, {"packageSha256", hash("initrd.gz")}}}};
    Json vm = {{"machine", profile == "ppc64le-tcg" ? "pseries-8.2" : "virt-8.2"},
               {"accelerator", profile == "ppc64le-tcg" ? "tcg" : "kvm"},
               {"firmwareSha256", hash("firmware")},
               {"kernelSha256", hash("kernel")},
               {"initrdSha256", hash("initrd.gz")}};
    if (std::filesystem::exists(input / "known_hosts")) {
      vm["sshUser"] = "root";
      vm["knownHostsSha256"] = hash("known_hosts");
    }
    Json artifact = {{"contract", contract},
                     {"contractSha256", lab_support::digest(contract)},
                     {"diskSha256", hash("disk.raw")},
                     {"platform", platform},
                     {"vm", vm}};
    Json lock = {{"apiVersion", "graphlab.artifacts/v1"},
                 {"workloads", {{"guest-linux", artifact}}}};
    Json ports = Json::object();
    for (auto port : {"data0", "mgmt0"})
      ports[port] = {{"role", std::string(port) == "data0" ? "data" : "management"},
                     {"medium", "ethernet"},
                     {"mtu", 1500}};
    Json topology = {
        {"apiVersion", "graphlab.topology/v2"},
        {"id", profile},
        {"artifactLock", lab_support::digest(lock)},
        {"backend", {{"kind", "linux-local"}, {"switchIsolation", "shared-ovs"}}},
        {"capture", {{"format", "pcapng"}, {"required", true}, {"scope", "all-data-edges"}}},
        {"nodes",
         {{"guest", {{"kind", "qemu"}, {"workload", "guest-linux"}, {"ports", ports}}},
          {"s1",
           {{"kind", "ovs-switch"},
            {"policy", {{"forwarding", "normal"}, {"priority", 4096}, {"rstp", true}}},
            {"ports",
             {{"p1",
               {{"role", "data"},
                {"medium", "ethernet"},
                {"mtu", 1500},
                {"vlan", {{"access", 100}}}}}}}}}}},
        {"edges", Json::array({{{"id", "guest-link"},
                                {"endpoints", Json::array({"guest:data0", "s1:p1"})}}})},
        {"management",
         {{"networks",
           {{"control",
             {{"subnet", "172.31.243.0/24"},
              {"gateway", "172.31.243.1"},
              {"dynamicPool", "172.31.243.128/25"},
              {"externalAccess", false}}}}}}},
        {"managementAttachments", Json::array({{{"endpoint", "guest:mgmt0"},
                                                {"network", "control"},
                                                {"address", "172.31.243.10/24"}}})}};
    auto result = lab_support::validate(topology, lock);
    if (!result)
      throw std::runtime_error(result.error().message);
    graphlab::capture::atomic_json(out / "artifacts.lock.json", lock);
    graphlab::capture::atomic_json(out / "topology.yaml", topology);
    std::ofstream sums(out / "SHA256SUMS");
    for (auto file :
         {"kernel", "firmware", "initrd.gz", "disk.raw", "known_hosts", "modules.tar.gz"})
      if (std::filesystem::exists(input / file))
        sums << hash(file).substr(7) << "  artifacts/" << file << '\n';
    std::cout << result->hash << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
