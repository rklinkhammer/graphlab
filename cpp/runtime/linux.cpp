#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <graphlab/runtime.hpp>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace graphlab::runtime {
Json LinuxBackend::capture_plan(const Json &r) { return capture::plan(r, capture_root_); }
Json LinuxBackend::capture_control(const Json &r, const std::string &op) {
  return capture::control(r, op);
}
namespace detail {
Json lookup_link(const ProcessResult &result, const std::string &name) {
  if (result.code)
    throw Failure("link_inventory_unavailable", 503);
  auto links = Json::parse(result.output);
  if (!links.is_array())
    throw Failure("invalid_link_inventory", 503);
  for (const auto &entry : links)
    if (entry.at("ifname") == name)
      return entry;
  return nullptr;
}
bool lookup_present(const ProcessResult &result) {
  if (result.code)
    throw Failure("ovs_inventory_unavailable", 503);
  return result.output.find_first_not_of(" \t\r\n") != std::string::npos;
}
void verify_namespace(const Json &expected, const Json &container, std::uint64_t inode) {
  if (!expected.is_null() && (expected.at("namespaceInode") != std::to_string(inode) ||
                              expected.at("containerId") != container.at("Id")))
    throw Failure("namespace_identity_changed", 409);
}
} // namespace detail
namespace {
constexpr auto api = "/v1.52";
std::string text(const Json &j, const char *key) { return j.at(key).get<std::string>(); }
std::string owner(const Json &run, const Json &resource) {
  return text(run, "id") + ":" + text(resource, "key");
}
Json labels(const Json &run, const Json &resource) {
  return {{"graphlab.run", run["id"]},
          {"graphlab.resource", resource["key"]},
          {"graphlab.generation", run["generation"]}};
}
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  if (s.size() > 1 && s.front() == '"' && s.back() == '"')
    s = s.substr(1, s.size() - 2);
  return s;
}
std::string command(std::vector<std::string> args, int timeout = 10) {
  auto r = process(args, timeout);
  if (r.code)
    throw Failure("backend_command_failed", 503);
  return trim(r.output);
}
void disable_ipv6(const std::string &name) {
  (void)name;
#ifdef __linux__
  std::ofstream control("/proc/sys/net/ipv6/conf/" + name + "/disable_ipv6");
  control << "1\n";
  control.flush();
  if (!control)
    throw Failure("interface_ipv6_policy_failed");
#endif
}
Json inspect_container(const Json &run, const Json &resource, bool absent = false) {
  auto name = resource_name(run, text(resource, "key"));
  auto response = docker_request("GET", std::string(api) + "/containers/" + name + "/json");
  if (response.status == 404 && absent)
    return nullptr;
  if (response.status != 200)
    throw Failure("container_unavailable", 503);
  auto value = Json::parse(response.body);
  auto expected = labels(run, resource);
  for (const auto &[k, v] : expected.items())
    if (!value["Config"]["Labels"].contains(k) || value["Config"]["Labels"][k] != v)
      throw Failure("container_ownership_conflict", 409);
  if (resource.contains("identity") && resource["identity"].contains("id") &&
      resource["identity"]["id"] != value["Id"])
    throw Failure("container_identity_changed", 409);
  return value;
}
Json node_resource(const Json &run, const std::string &node) {
  for (const auto &r : run["resources"])
    if (r["key"] == "node/" + node)
      return r;
  throw Failure("missing_node_resource");
}
struct Namespace {
  int fd = -1;
  explicit Namespace(const Json &container, const Json &expected = nullptr) {
    int pid = container["State"]["Pid"];
    if (pid <= 0 || container["State"]["Running"] != true)
      throw Failure("container_not_running");
    auto path = "/proc/" + std::to_string(pid) + "/ns/net";
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw Failure("namespace_unavailable");
    try {
      struct stat info{};
      if (fstat(fd, &info))
        throw Failure("namespace_unavailable");
      detail::verify_namespace(expected, container, info.st_ino);
    } catch (...) {
      close(fd);
      fd = -1;
      throw;
    }
  }
  ~Namespace() {
    if (fd >= 0)
      close(fd);
  }
};
Json link(const std::string &name, const std::vector<std::string> &prefix = {}) {
  auto args = prefix;
  args.insert(args.end(), {"/usr/sbin/ip", "-j", "link", "show"});
  return detail::lookup_link(process(args), name);
}
void ip(std::vector<std::string> args, const std::vector<std::string> &prefix = {}) {
  auto command_args = prefix;
  command_args.push_back("/usr/sbin/ip");
  command_args.insert(command_args.end(), args.begin(), args.end());
  command(command_args);
}
std::vector<std::string> ns_prefix(const Namespace &ns) {
  return {"/usr/bin/nsenter", "--net=/proc/self/fd/" + std::to_string(ns.fd), "--"};
}
void verify_edge_namespaces(const Json &run, const Json &r, bool absent) {
  for (int index = 0; index < 2; ++index) {
    auto endpoint = r["configuration"]["endpoints"][index].get<std::string>();
    auto nr = node_resource(run, endpoint.substr(0, endpoint.find(':')));
    if (nr["kind"] != "container")
      continue;
    auto c = inspect_container(run, nr, absent);
    if (absent && (c.is_null() || c["State"]["Running"] != true))
      continue;
    auto expected = r.contains("identity") ? r["identity"]["endpoints"][index] : Json(nullptr);
    Namespace ns(c, expected);
    if (inspect_container(run, nr)["State"]["Pid"] != c["State"]["Pid"])
      throw Failure("namespace_identity_changed", 409);
  }
}
bool bridge_present(const std::string &name) {
  return detail::lookup_present(process(
      {"/usr/bin/ovs-vsctl", "--timeout=5", "--if-exists", "get", "Bridge", name, "_uuid"}));
}
void verify_bridge(const Json &run, const Json &r, bool absent = false) {
  auto name = resource_name(run, text(r, "key"));
  if (!bridge_present(name)) {
    if (absent)
      return;
    throw Failure("bridge_missing");
  }
  auto marker = command(
      {"/usr/bin/ovs-vsctl", "--timeout=5", "get", "Bridge", name, "external_ids:graphlab-owner"});
  if (marker != owner(run, r))
    throw Failure("bridge_ownership_conflict", 409);
}
std::string mac(const Json &run, const Json &resource, int index) {
  auto hash = lab_support::digest(Json::array({run["id"], resource["key"], index}));
  std::string result = "02";
  for (int i = 0; i < 5; ++i)
    result += ":" + hash.substr(7 + i * 2, 2);
  return result;
}
void check_link(const Json &found, const std::string &marker, const Json &expected = nullptr,
                const std::string &intent_mac = "") {
  if (found.is_null())
    return;
  if (found.value("ifalias", "") != marker &&
      !(expected.is_null() && found.value("ifalias", "").empty() && !intent_mac.empty() &&
        found.value("address", "") == intent_mac))
    throw Failure("link_ownership_conflict", 409);
  if (!expected.is_null() && expected.contains("ifindex") &&
      found["ifindex"] != expected["ifindex"])
    throw Failure("link_identity_changed", 409);
}
Json node_exec(const Json &run, const Json &r, const std::string &action) {
  auto c = inspect_container(run, r);
  auto created =
      docker_json("POST", std::string(api) + "/containers/" + text(c, "Id") + "/exec",
                  {{"AttachStdout", true},
                   {"AttachStderr", true},
                   {"Cmd", Json::array({"/usr/local/bin/lab-node", "control", action})}});
  auto response =
      docker_request("POST", std::string(api) + "/exec/" + text(created, "Id") + "/start",
                     {{"Detach", false}, {"Tty", false}});
  if (response.status != 200)
    throw Failure("node_control_failed");
  auto info = docker_json("GET", std::string(api) + "/exec/" + text(created, "Id") + "/json");
  if (info["Running"] != false || info["ExitCode"] != 0)
    throw Failure("node_gate_failed");
  std::string output;
  for (std::size_t i = 0; i < response.body.size();) {
    if (i + 8 > response.body.size())
      throw Failure("invalid_exec_stream");
    std::uint32_t n = 0;
    for (int k = 4; k < 8; ++k)
      n = (n << 8) | static_cast<unsigned char>(response.body[i + k]);
    i += 8;
    if (i + n > response.body.size())
      throw Failure("invalid_exec_stream");
    output += response.body.substr(i, n);
    i += n;
  }
  return Json::parse(output);
}
} // namespace
void LinuxBackend::preflight(const Json &t, const Json &artifacts) {
#ifndef __linux__
  (void)t;
  (void)artifacts;
  throw Failure("linux_runtime_required");
#else
  if (geteuid() != 0)
    throw Failure("root_agent_required");
  auto version = docker_json("GET", "/version");
  if (version.value("ApiVersion", "") < "1.52")
    throw Failure("docker_api_1_52_required");
  command({"/usr/bin/ovs-vsctl", "--timeout=5", "show"});
  for (const auto &container : docker_json("GET", std::string(api) + "/containers/json?all=true"))
    if (container["Labels"].contains("graphlab.run"))
      throw Failure("existing_graphlab_resources_require_recovery", 409);
  for (const auto &network : docker_json("GET", std::string(api) + "/networks"))
    if (network["Labels"].contains("graphlab.run"))
      throw Failure("existing_graphlab_resources_require_recovery", 409);
  auto bridges = command({"/usr/bin/ovs-vsctl", "--timeout=5", "--format=json",
                          "--columns=external_ids", "list", "Bridge"});
  if (bridges.find("\"graphlab-owner\"") != std::string::npos)
    throw Failure("existing_graphlab_resources_require_recovery", 409);
  utsname host{};
  uname(&host);
  std::string platform = std::string(host.machine) == "aarch64" ? "linux/arm64" : "linux/amd64";
  std::uint64_t memory_mib = 0, cpus = 0;
  for (const auto &[id, n] : t["nodes"].items())
    if (n["kind"] == "docker") {
      const auto &limits = artifacts["workloads"][text(n, "workload")]["contract"]["resources"];
      memory_mib += limits["memoryMiB"].get<std::uint64_t>();
      cpus += limits["cpus"].get<std::uint64_t>();
    }
  std::ifstream memory("/proc/meminfo");
  std::string line;
  std::uint64_t available_kib = 0;
  while (std::getline(memory, line))
    if (line.starts_with("MemAvailable:")) {
      std::istringstream value(line.substr(13));
      value >> available_kib;
    }
  if (memory_mib * 1024 > available_kib * 4 / 5 || cpus > std::thread::hardware_concurrency())
    throw Failure("host_resource_budget_exceeded");
  auto routes = Json::parse(command({"/usr/sbin/ip", "-4", "-j", "route", "show"}));
  auto network = [](std::string value) {
    auto slash = value.find('/');
    int prefix = slash == std::string::npos ? 32 : std::stoi(value.substr(slash + 1));
    in_addr address{};
    if (inet_pton(AF_INET, value.substr(0, slash).c_str(), &address) != 1 || prefix < 0 ||
        prefix > 32)
      throw Failure("invalid_host_route");
    return std::pair{ntohl(address.s_addr), prefix == 0 ? 0u : 0xffffffffu << (32 - prefix)};
  };
  for (const auto &[id, n] : t["management"]["networks"].items()) {
    auto [address, mask] = network(n["subnet"]);
    for (const auto &route : routes) {
      auto destination = route.value("dst", "default");
      if (destination == "default" || destination == "0.0.0.0/0")
        continue;
      auto [other, other_mask] = network(destination);
      if ((address & other_mask) == (other & other_mask) || (other & mask) == (address & mask))
        throw Failure("management_host_route_conflict");
    }
  }
  for (const auto &[id, n] : t["nodes"].items())
    if (n["kind"] == "docker") {
      auto artifact = artifacts["workloads"][text(n, "workload")];
      if (artifact["platform"] != platform)
        throw Failure("artifact_platform_mismatch");
      auto reference = text(artifact, "image");
      auto identifier = reference.starts_with("graphlab.local/")
                            ? reference.substr(reference.find('@') + 1)
                            : reference;
      auto image = docker_json("GET", std::string(api) + "/images/" + identifier + "/json");
      bool matches = image["Id"] == reference.substr(reference.find('@') + 1);
      if (image.contains("RepoDigests") && image["RepoDigests"].is_array())
        for (const auto &d : image["RepoDigests"])
          matches = matches || d == reference;
      if (!matches)
        throw Failure("image_digest_mismatch");
      if (image.value("Architecture", "") != (platform == "linux/arm64" ? "arm64" : "amd64"))
        throw Failure("image_architecture_mismatch");
      if (image["Config"]["Labels"].value("graphlab.gate-protocol", "") != "1")
        throw Failure("cpp_gate_image_required");
      if (t["capture"]["required"] == true &&
          image["Config"]["Labels"].value("graphlab.traffic-lease", "") != "1")
        throw Failure("capture_requires_lease_capable_image");
    }
#endif
}
Json LinuxBackend::prepare(const Json &run, const Json &r) {
  auto kind = text(r, "kind"), name = resource_name(run, text(r, "key"));
  const auto &config = r["configuration"];
  if (kind == "network") {
    auto prior = docker_request("GET", std::string(api) + "/networks/" + name);
    if (prior.status != 404)
      throw Failure("network_name_conflict", 409);
    auto created = docker_json("POST", std::string(api) + "/networks/create",
                               {{"Name", name},
                                {"Driver", "bridge"},
                                {"Internal", true},
                                {"EnableIPv6", false},
                                {"Labels", labels(run, r)},
                                {"IPAM",
                                 {{"Driver", "default"},
                                  {"Config", Json::array({{{"Subnet", config["subnet"]},
                                                           {"IPRange", config["dynamicPool"]},
                                                           {"Gateway", config["gateway"]}}})}}}});
    return {{"id", created["Id"]}};
  }
  if (kind == "bridge") {
    if (bridge_present(name) || !link(name).is_null())
      throw Failure("bridge_name_conflict", 409);
    command(
        {"/usr/bin/ovs-vsctl", "--timeout=5", "add-br", name, "--", "set", "Bridge", name,
         "external_ids:graphlab-owner=" + owner(run, r),
         "rstp_enable=" + std::string(config["policy"]["rstp"] == true ? "true" : "false"),
         "other_config:rstp-priority=" + std::to_string(config["policy"]["priority"].get<int>()),
         "fail_mode=standalone"});
    verify_bridge(run, r);
    disable_ipv6(name);
    return {
        {"name", name},
        {"uuid", command({"/usr/bin/ovs-vsctl", "--timeout=5", "get", "Bridge", name, "_uuid"})}};
  }
  if (kind == "container") {
    auto prior = docker_request("GET", std::string(api) + "/containers/" + name + "/json");
    if (prior.status != 404)
      throw Failure("container_name_conflict", 409);
    auto artifact = run["artifacts"]["workloads"][text(config, "workload")];
    auto reference = text(artifact, "image");
    auto image = reference.starts_with("graphlab.local/")
                     ? reference.substr(reference.find('@') + 1)
                     : reference;
    auto limits = artifact["contract"]["resources"];
    Json body = {
        {"Image", image},
        {"Labels", labels(run, r)},
        {"Entrypoint", Json::array({"/usr/local/bin/lab-node"})},
        {"Cmd", Json::array()},
        {"HostConfig",
         {{"NetworkMode", "none"},
          {"ReadonlyRootfs", true},
          {"CapDrop", Json::array({"ALL"})},
          {"SecurityOpt", Json::array({"no-new-privileges:true"})},
          {"Memory", limits["memoryMiB"].get<std::int64_t>() * 1024 * 1024},
          {"NanoCpus", limits["cpus"].get<std::int64_t>() * 1000000000},
          {"PidsLimit", 32},
          {"Tmpfs", {{"/run", "rw,nosuid,nodev,noexec,size=1m"}}},
          {"Sysctls", {{"net.ipv4.ip_forward", "0"}, {"net.ipv6.conf.all.disable_ipv6", "1"}}}}}};
    Json attachments = Json::array();
    for (const auto &a : run["topology"]["managementAttachments"])
      if (text(a, "endpoint").starts_with(text(r, "logical") + ":"))
        attachments.push_back(a);
    auto endpoint_config = [&](const Json &a) {
      auto address = text(a, "address");
      address = address.substr(0, address.find('/'));
      return Json{{"IPAMConfig", {{"IPv4Address", address}}},
                  {"DriverOpts",
                   {{"com.docker.network.endpoint.ifname",
                     text(a, "endpoint").substr(text(a, "endpoint").find(':') + 1)}}}};
    };
    if (!attachments.empty()) {
      auto network = resource_name(run, "management/" + text(attachments[0], "network"));
      body["HostConfig"]["NetworkMode"] = network;
      body["NetworkingConfig"]["EndpointsConfig"][network] = endpoint_config(attachments[0]);
    }
    auto created = docker_json("POST", std::string(api) + "/containers/create?name=" + name, body);
    auto id = text(created, "Id");
    docker_json("POST", std::string(api) + "/containers/" + id + "/start");
    for (std::size_t attachment_index = 1; attachment_index < attachments.size();
         ++attachment_index) {
      const auto &a = attachments[attachment_index];
      auto network = resource_name(run, "management/" + text(a, "network"));
      auto address = text(a, "address");
      address = address.substr(0, address.find('/'));
      docker_json("POST", std::string(api) + "/networks/" + network + "/connect",
                  {{"Container", id},
                   {"EndpointConfig",
                    {{"IPAMConfig", {{"IPv4Address", address}}},
                     {"DriverOpts",
                      {{"com.docker.network.endpoint.ifname",
                        text(a, "endpoint").substr(text(a, "endpoint").find(':') + 1)}}}}}});
    }
    auto current = inspect_container(run, r);
    auto gate = node_exec(run, r, "status");
    if (gate["state"] != "held")
      throw Failure("node_not_gated");
    return {{"id", id}, {"pid", current["State"]["Pid"]}, {"gate", "held"}};
  }
  auto endpoints = config["endpoints"];
  std::string names[2] = {resource_name(run, text(r, "key") + "/0"),
                          resource_name(run, text(r, "key") + "/1")};
  if (!link(names[0]).is_null() || !link(names[1]).is_null())
    throw Failure("link_name_conflict", 409);
  ip({"link", "add", "name", names[0], "address", mac(run, r, 0), "type", "veth", "peer", "name",
      names[1], "address", mac(run, r, 1)});
  for (int index = 0; index < 2; ++index)
    ip({"link", "set", "dev", names[index], "alias", owner(run, r)});
  for (const auto &physical : names)
    disable_ipv6(physical);
  Json result = {{"endpoints", Json::array()}};
  for (int index = 0; index < 2; ++index) {
    auto endpoint = endpoints[index].get<std::string>();
    auto colon = endpoint.find(':');
    auto node = endpoint.substr(0, colon), port = endpoint.substr(colon + 1);
    auto nr = node_resource(run, node);
    const auto &nc = nr["configuration"];
    int mtu = nc["ports"][port]["mtu"];
    if (nr["kind"] == "bridge") {
      verify_bridge(run, nr);
      ip({"link", "set", "dev", names[index], "mtu", std::to_string(mtu)});
      auto vlan = nc["ports"][port]["vlan"];
      std::vector<std::string> args = {"/usr/bin/ovs-vsctl",
                                       "--timeout=5",
                                       "add-port",
                                       resource_name(run, text(nr, "key")),
                                       names[index],
                                       "--",
                                       "set",
                                       "Port",
                                       names[index],
                                       "external_ids:graphlab-owner=" + owner(run, r)};
      if (vlan.contains("access")) {
        args.push_back("vlan_mode=access");
        args.push_back("tag=" + std::to_string(vlan["access"].get<int>()));
      } else {
        args.push_back("vlan_mode=trunk");
        std::string trunks = "trunks=";
        for (const auto &v : vlan["trunk"]) {
          if (trunks.back() != '=')
            trunks += ',';
          trunks += std::to_string(v.get<int>());
        }
        args.push_back(trunks);
      }
      command(args);
      auto observed = link(names[index]);
      check_link(observed, owner(run, r));
      result["endpoints"].push_back(
          {{"name", names[index]}, {"ifindex", observed["ifindex"]}, {"namespace", "host"}});
    } else {
      auto container = inspect_container(run, nr);
      Namespace ns(container);
      auto prefix = ns_prefix(ns);
      if (inspect_container(run, nr)["State"]["Pid"] != container["State"]["Pid"])
        throw Failure("namespace_identity_changed");
      if (!link(port, prefix).is_null())
        throw Failure("container_port_conflict", 409);
      ip({"link", "set", "dev", names[index], "netns", "/proc/self/fd/" + std::to_string(ns.fd)});
      ip({"link", "set", "dev", names[index], "name", port}, prefix);
      ip({"link", "set", "dev", port, "mtu", std::to_string(mtu)}, prefix);
      ip({"link", "set", "dev", port, "alias", owner(run, r)}, prefix);
      if (nc.contains("addresses") && nc["addresses"].contains(port))
        ip({"address", "add", nc["addresses"][port], "dev", port}, prefix);
      auto observed = link(port, prefix);
      check_link(observed, owner(run, r));
      struct stat st{};
      if (fstat(ns.fd, &st))
        throw Failure("namespace_unavailable");
      result["endpoints"].push_back({{"name", port},
                                     {"ifindex", observed["ifindex"]},
                                     {"namespaceInode", std::to_string(st.st_ino)},
                                     {"containerId", container["Id"]}});
    }
  }
  return result;
}
void LinuxBackend::remove(const Json &run, const Json &r) {
  auto kind = text(r, "kind"), name = resource_name(run, text(r, "key"));
  if (kind == "container") {
    auto c = inspect_container(run, r, true);
    if (!c.is_null())
      docker_json("DELETE", std::string(api) + "/containers/" + text(c, "Id") + "?force=true");
    return;
  }
  if (kind == "network") {
    auto response = docker_request("GET", std::string(api) + "/networks/" + name);
    if (response.status == 404)
      return;
    if (response.status != 200)
      throw Failure("network_unavailable");
    auto n = Json::parse(response.body);
    auto expected_labels = labels(run, r);
    for (const auto &[key, value] : expected_labels.items())
      if (n["Labels"].value(key, Json()) != value)
        throw Failure("network_ownership_conflict", 409);
    if (r.contains("identity") && r["identity"]["id"] != n["Id"])
      throw Failure("network_identity_changed", 409);
    docker_json("DELETE", std::string(api) + "/networks/" + text(n, "Id"));
    return;
  }
  if (kind == "bridge") {
    if (!bridge_present(name))
      return;
    verify_bridge(run, r);
    if (r.contains("identity") &&
        r["identity"]["uuid"] !=
            command({"/usr/bin/ovs-vsctl", "--timeout=5", "get", "Bridge", name, "_uuid"}))
      throw Failure("bridge_identity_changed", 409);
    command({"/usr/bin/ovs-vsctl", "--timeout=5", "del-br", name});
    return;
  }
  verify_edge_namespaces(run, r, true);
  for (int index = 0; index < 2; ++index) {
    auto endpoint = r["configuration"]["endpoints"][index].get<std::string>();
    auto colon = endpoint.find(':');
    auto node = endpoint.substr(0, colon), port = endpoint.substr(colon + 1);
    auto nr = node_resource(run, node);
    auto physical = resource_name(run, text(r, "key") + "/" + std::to_string(index));
    auto expected = r.contains("identity") ? r["identity"]["endpoints"][index] : Json(nullptr);
    if (nr["kind"] == "bridge") {
      auto found = link(physical);
      check_link(found, owner(run, r), expected, mac(run, r, index));
      auto row = process({"/usr/bin/ovs-vsctl", "--timeout=5", "--if-exists", "get", "Port",
                          physical, "external_ids:graphlab-owner"});
      if (detail::lookup_present(row)) {
        if (trim(row.output) != owner(run, r))
          throw Failure("port_ownership_conflict", 409);
        command({"/usr/bin/ovs-vsctl", "--timeout=5", "del-port", physical});
      }
      if (!found.is_null())
        ip({"link", "delete", "dev", physical});
    } else {
      auto c = inspect_container(run, nr, true);
      if (!c.is_null() && c["State"]["Running"] == true) {
        Namespace ns(c, expected);
        if (inspect_container(run, nr)["State"]["Pid"] != c["State"]["Pid"])
          throw Failure("namespace_identity_changed", 409);
        auto prefix = ns_prefix(ns);
        auto found = link(port, prefix);
        check_link(found, owner(run, r), expected, mac(run, r, index));
        if (!found.is_null())
          ip({"link", "delete", "dev", port}, prefix);
      }
      auto leftover = link(physical);
      check_link(leftover, owner(run, r), nullptr, mac(run, r, index));
      if (!leftover.is_null())
        ip({"link", "delete", "dev", physical});
    }
  }
}
void LinuxBackend::activate(const Json &run) {
  for (const auto &r : run["resources"])
    if (r["kind"] == "edge")
      verify_edge_namespaces(run, r, false);
  for (const auto &r : run["resources"]) {
    if (r["kind"] == "bridge") {
      verify_bridge(run, r);
      ip({"link", "set", "dev", resource_name(run, text(r, "key")), "up"});
    }
    if (r["kind"] != "edge")
      continue;
    for (int index = 0; index < 2; ++index) {
      auto endpoint = r["configuration"]["endpoints"][index].get<std::string>();
      auto colon = endpoint.find(':');
      auto nr = node_resource(run, endpoint.substr(0, colon));
      auto port = endpoint.substr(colon + 1);
      auto physical = resource_name(run, text(r, "key") + "/" + std::to_string(index));
      auto expected = r["identity"]["endpoints"][index];
      if (nr["kind"] == "bridge") {
        check_link(link(physical), owner(run, r), expected);
        ip({"link", "set", "dev", physical, "up"});
      } else {
        auto c = inspect_container(run, nr);
        Namespace ns(c, expected);
        if (inspect_container(run, nr)["State"]["Pid"] != c["State"]["Pid"])
          throw Failure("namespace_identity_changed", 409);
        auto prefix = ns_prefix(ns);
        check_link(link(port, prefix), owner(run, r), expected);
        ip({"link", "set", "dev", port, "up"}, prefix);
      }
    }
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    bool ready = true;
    for (const auto &r : run["resources"])
      if (r["kind"] == "edge")
        for (int index = 0; index < 2; ++index) {
          auto ep = r["configuration"]["endpoints"][index].get<std::string>();
          auto nr = node_resource(run, ep.substr(0, ep.find(':')));
          if (nr["kind"] != "bridge" || nr["configuration"]["policy"]["rstp"] != true)
            continue;
          auto status = command({"/usr/bin/ovs-vsctl", "--timeout=5", "get", "Port",
                                 resource_name(run, text(r, "key") + "/" + std::to_string(index)),
                                 "rstp_status"});
          std::transform(status.begin(), status.end(), status.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          ready = ready && (status.find("forwarding") != std::string::npos ||
                            (status.find("discarding") != std::string::npos &&
                             (status.find("alternate") != std::string::npos ||
                              status.find("backup") != std::string::npos)));
        }
    if (ready)
      return;
    if (std::chrono::steady_clock::now() > deadline)
      throw Failure("rstp_convergence_timeout");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
void LinuxBackend::gate(const Json &run, const std::string &action) {
  for (const auto &r : run["resources"])
    if (r["kind"] == "container" && r.value("state", "") != "removed") {
      bool leased = run["topology"]["capture"]["required"] == true;
      auto operation = leased && action == "release" ? "release-lease"
                       : action == "renew"           ? "renew-lease"
                                                     : action;
      auto result = node_exec(run, r, operation);
      if (result["state"] != (action == "release" || action == "renew" ? "released" : "held"))
        throw Failure("gate_acknowledgement_failed");
      if (leased && (action == "release" || action == "renew") &&
          !result.value("leaseActive", false))
        throw Failure("traffic_lease_not_acknowledged");
      const auto &configuration = r["configuration"];
      if (action == "release" && configuration.contains("addresses") &&
          configuration["addresses"].contains("data0") && !result.value("dataReady", false))
        throw Failure("workload_data_readiness_failed");
    }
}
Json LinuxBackend::observe(const Json &run) {
  Json nodes = Json::array();
  if (run["state"] != "destroyed")
    for (const auto &r : run["resources"])
      if (r["kind"] == "container")
        nodes.push_back({{"id", r["logical"]},
                         {"gate", node_exec(run, r, "status")},
                         {"containerId", inspect_container(run, r)["Id"]}});
  return {{"observedAt", console::timestamp()},
          {"nodes", nodes},
          {"captureCoverage", "unavailable-development-mode"}};
}
} // namespace graphlab::runtime
