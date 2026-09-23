#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>
#include <graphlab/capture.hpp>
#include <graphlab/qemu.hpp>
#include <graphlab/runtime.hpp>
#include <graphlab/telemetry.hpp>
#include <sstream>
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
std::string physical(const Json &run, const Json &r, int index) {
  auto endpoints = r["configuration"]["endpoints"];
  for (int i = 0; i < 2; ++i) {
    auto endpoint = endpoints[i].get<std::string>();
    auto requested = endpoints[index].get<std::string>();
    auto requested_kind = node_resource(run, requested.substr(0, requested.find(':')))["kind"];
    if (i != index && requested_kind != "bridge")
      continue;
    auto colon = endpoint.find(':');
    if (node_resource(run, endpoint.substr(0, colon))["kind"] == "qemu")
      return qemu::tap_name(run, endpoint.substr(0, colon), endpoint.substr(colon + 1));
  }
  return resource_name(run, text(r, "key") + "/" + std::to_string(index));
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
bool direct_guest(const Json &run, const Json &r) {
  bool guest = false;
  for (const auto &ep : r["configuration"]["endpoints"]) {
    auto text = ep.get<std::string>();
    auto kind = node_resource(run, text.substr(0, text.find(':')))["kind"];
    if (kind == "bridge")
      return false;
    guest = guest || kind == "qemu";
  }
  return guest;
}
Json attachment_resource(const Json &r) { return {{"key", text(r, "key") + "/attachment"}}; }
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
  qemu::preflight(t, artifacts, state_root_);
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
    if (n["kind"] == "docker" || n["kind"] == "qemu") {
      const auto &limits = artifacts["workloads"][text(n, "workload")]["contract"]["resources"];
      memory_mib += limits["memoryMiB"].get<std::uint64_t>();
      if (n["kind"] == "qemu")
        memory_mib += 512;
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
  if (kind == "qemu")
    return qemu::prepare(run, r, state_root_);
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
  if (direct_guest(run, r)) {
    auto attachment = attachment_resource(r);
    auto bridge = resource_name(run, text(attachment, "key"));
    if (bridge_present(bridge) || !link(bridge).is_null())
      throw Failure("attachment_name_conflict");
    command({"/usr/bin/ovs-vsctl", "--timeout=5", "add-br", bridge, "--", "set", "Bridge", bridge,
             "external_ids:graphlab-owner=" + owner(run, attachment),
             "external_ids:graphlab-role=attachment", "rstp_enable=false", "fail_mode=standalone"});
    detail::checkpoint("attachment.bridge");
    verify_bridge(run, attachment);
    disable_ipv6(bridge);
    ip({"link", "set", bridge, "up"});
    Json result = {{"endpoints", Json::array()},
                   {"attachment",
                    {{"name", bridge},
                     {"uuid", command({"/usr/bin/ovs-vsctl", "get", "Bridge", bridge, "_uuid"})},
                     {"mode", "two-port-shared-ovs"},
                     {"ports", Json::array()}}}};
    for (int index = 0; index < 2; ++index) {
      auto ep = endpoints[index].get<std::string>();
      auto colon = ep.find(':');
      auto node = node_resource(run, ep.substr(0, colon));
      auto port = ep.substr(colon + 1);
      auto name = physical(run, r, index);
      auto mtu = std::to_string(node["configuration"]["ports"][port]["mtu"].get<int>());
      Json endpoint;
      if (node["kind"] == "qemu") {
        auto observed = link(name);
        check_link(observed, owner(run, r));
        if (observed.is_null())
          throw Failure("tap_missing");
        endpoint = {{"name", name},
                    {"ifindex", observed["ifindex"]},
                    {"namespace", "host"},
                    {"kind", "tap"}};
      } else {
        auto peer = resource_name(run, text(r, "key") + "/peer/" + std::to_string(index));
        if (!link(name).is_null() || !link(peer).is_null())
          throw Failure("link_name_conflict");
        ip({"link", "add", "name", name, "address", mac(run, r, index), "type", "veth", "peer",
            "name", peer, "address", mac(run, r, 2 + index)});
        detail::checkpoint("attachment.veth-created");
        ip({"link", "set", name, "alias", owner(run, r)});
        ip({"link", "set", peer, "alias", owner(run, r)});
        disable_ipv6(name);
        disable_ipv6(peer);
        auto container = inspect_container(run, node);
        Namespace ns(container);
        auto prefix = ns_prefix(ns);
        if (!link(port, prefix).is_null())
          throw Failure("container_port_conflict");
        if (inspect_container(run, node)["State"]["Pid"] != container["State"]["Pid"])
          throw Failure("namespace_identity_changed");
        ip({"link", "set", peer, "netns", "/proc/self/fd/" + std::to_string(ns.fd)});
        detail::checkpoint("attachment.veth-moved");
        ip({"link", "set", peer, "name", port}, prefix);
        ip({"link", "set", port, "mtu", mtu}, prefix);
        auto nc = node["configuration"];
        if (nc.contains("addresses") && nc["addresses"].contains(port))
          ip({"address", "add", nc["addresses"][port], "dev", port}, prefix);
        auto observed = link(port, prefix);
        struct stat st{};
        if (fstat(ns.fd, &st))
          throw Failure("namespace_unavailable");
        endpoint = {{"name", port},
                    {"ifindex", observed["ifindex"]},
                    {"namespaceInode", std::to_string(st.st_ino)},
                    {"containerId", container["Id"]}};
      }
      ip({"link", "set", name, "mtu", mtu});
      auto qos = command({"/usr/bin/ovs-vsctl", "--timeout=5", "--", "--id=@q", "create", "QoS",
                          "type=linux-noop", "external_ids:graphlab-owner=" + owner(run, r), "--",
                          "add-port", bridge, name, "--", "set", "Port", name,
                          "external_ids:graphlab-owner=" + owner(run, r), "qos=@q"});
      detail::checkpoint("attachment.qos." + std::to_string(index));
      if (node["kind"] == "qemu") {
        // Install an owned parent on a fresh TAP. Faults attach beneath it,
        // rather than replacing the kernel default or an unrelated root qdisc.
        command({"/usr/sbin/tc",
                 "qdisc",
                 "add",
                 "dev",
                 name,
                 "root",
                 "handle",
                 "7:",
                 "prio",
                 "bands",
                 "3",
                 "priomap",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0",
                 "0"});
      }

      if (node["kind"] == "qemu")
        command({"/usr/sbin/tc", "qdisc", "replace", "dev", name, "parent", "7:1", "handle",
                 "8:", "pfifo", "limit", "1000"});
      auto host = link(name);
      result["attachment"]["ports"].push_back(
          {{"name", name},
           {"ifindex", host["ifindex"]},
           {"namespace", "host"},
           {"qosUuid", qos},
           {"kind", node["kind"] == "qemu" ? "tap" : "attachment-veth"}});
      if (node["kind"] == "qemu") {
        result["attachment"]["ports"].back()["faultParent"] = "7:1";
        result["attachment"]["ports"].back()["faultRoot"] = "7:";
        result["attachment"]["ports"].back()["faultNeutral"] = "8:";
      }
      result["endpoints"].push_back(endpoint);
      detail::checkpoint("attachment.port." + std::to_string(index));
    }
    return result;
  }
  for (int i = 0; i < 2; ++i) {
    auto ep = endpoints[i].get<std::string>();
    auto colon = ep.find(':');
    auto guest = node_resource(run, ep.substr(0, colon));
    if (guest["kind"] != "qemu")
      continue;
    auto other = endpoints[1 - i].get<std::string>();
    auto split = other.find(':');
    auto bridge = node_resource(run, other.substr(0, split));
    if (bridge["kind"] != "bridge")
      throw Failure("unsupported_guest_attachment");
    verify_bridge(run, bridge);
    auto tap = qemu::tap_name(run, ep.substr(0, colon), ep.substr(colon + 1));
    auto observed = link(tap);
    check_link(observed, owner(run, r));
    if (observed.is_null())
      throw Failure("tap_missing");
    ip({"link", "set", tap, "mtu",
        std::to_string(guest["configuration"]["ports"][ep.substr(colon + 1)]["mtu"].get<int>())});
    auto vlan = bridge["configuration"]["ports"][other.substr(split + 1)]["vlan"];
    std::vector<std::string> args = {"/usr/bin/ovs-vsctl",
                                     "--timeout=5",
                                     "add-port",
                                     resource_name(run, text(bridge, "key")),
                                     tap,
                                     "--",
                                     "set",
                                     "Port",
                                     tap,
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
    Json endpoint = {
        {"name", tap}, {"ifindex", observed["ifindex"]}, {"namespace", "host"}, {"kind", "tap"}};
    return {{"endpoints", Json::array({endpoint, endpoint})}};
  }
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
  if (kind == "qemu") {
    qemu::remove(run, r, state_root_);
    return;
  }
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
  if (direct_guest(run, r)) {
    auto attachment = attachment_resource(r);
    auto bridge = resource_name(run, text(attachment, "key"));
    verify_bridge(run, attachment, true);
    if (bridge_present(bridge) && r.contains("identity") &&
        command({"/usr/bin/ovs-vsctl", "get", "Bridge", bridge, "_uuid"}) !=
            r["identity"]["attachment"]["uuid"].get<std::string>())
      throw Failure("attachment_identity_changed");
    if (bridge_present(bridge)) {
      std::istringstream ports(command({"/usr/bin/ovs-vsctl", "list-ports", bridge}));
      std::string port;
      while (ports >> port) {
        if ((port != physical(run, r, 0) && port != physical(run, r, 1)) ||
            command({"/usr/bin/ovs-vsctl", "get", "Port", port, "external_ids:graphlab-owner"}) !=
                owner(run, r))
          throw Failure("attachment_port_ownership_conflict");
        auto qos = command({"/usr/bin/ovs-vsctl", "get", "Port", port, "qos"});
        if (command({"/usr/bin/ovs-vsctl", "get", "QoS", qos, "external_ids:graphlab-owner"}) !=
            owner(run, r))
          throw Failure("attachment_qos_ownership_conflict");
        if (r.contains("identity"))
          for (const auto &expected : r["identity"]["attachment"]["ports"])
            if (expected["name"] == port && expected["qosUuid"] != qos)
              throw Failure("attachment_qos_identity_changed");
      }
    }
    for (int i = 0; i < 2; ++i) {
      auto ep = r["configuration"]["endpoints"][i].get<std::string>();
      auto nr = node_resource(run, ep.substr(0, ep.find(':')));
      auto name = physical(run, r, i);
      auto found = link(name);
      auto expected =
          r.contains("identity") ? r["identity"]["attachment"]["ports"][i] : Json(nullptr);
      check_link(found, owner(run, r), expected, nr["kind"] == "container" ? mac(run, r, i) : "");
      if (nr["kind"] == "container" && !found.is_null())
        ip({"link", "delete", name});
    }
    if (bridge_present(bridge))
      command({"/usr/bin/ovs-vsctl", "--timeout=5", "del-br", bridge});
    auto rows =
        command({"/usr/bin/ovs-vsctl", "--data=bare", "--no-heading", "--columns=_uuid", "find",
                 "QoS", "external_ids:graphlab-owner=" + Json(owner(run, r)).dump()});
    std::istringstream ids(rows);
    std::string id;
    while (ids >> id) {
      if (!command({"/usr/bin/ovs-vsctl", "--data=bare", "--no-heading", "--columns=name", "find",
                    "Port", "qos=" + id})
               .empty())
        throw Failure("attachment_qos_still_referenced");
      command({"/usr/bin/ovs-vsctl", "destroy", "QoS", id});
    }
    return; // TAP lifetime belongs to the guest node, after attachment teardown.
  }
  for (int index = 0; index < 2; ++index) {
    auto endpoint = r["configuration"]["endpoints"][index].get<std::string>();
    auto colon = endpoint.find(':');
    auto node = endpoint.substr(0, colon), port = endpoint.substr(colon + 1);
    auto nr = node_resource(run, node);
    auto physical = ::graphlab::runtime::physical(run, r, index);
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
    } else if (nr["kind"] != "qemu") {
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
      auto physical = ::graphlab::runtime::physical(run, r, index);
      auto expected = r["identity"]["endpoints"][index];
      if (direct_guest(run, r) && nr["kind"] == "container") {
        auto found = link(physical);
        if (found.is_null())
          throw Failure("attachment_link_missing");
        check_link(found, owner(run, r));
        ip({"link", "set", physical, "up"});
      }
      if (nr["kind"] == "bridge" || nr["kind"] == "qemu") {
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
                                 physical(run, r, index), "rstp_status"});
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
  // Inspect every Docker peer before releasing any node (including QEMU).
  // Image labels are a preflight hint, not proof of the live wire protocol.
  if (action == "release")
    for (const auto &r : run["resources"])
      if (r["kind"] == "container" && r.value("state", "") != "removed") {
        auto version = lab_support::gate_protocol_minor(node_exec(run, r, "status"));
        if (!version)
          throw Failure(version.error().code);
      }
  for (const auto &r : run["resources"])
    if (r["kind"] == "qemu" && r.value("state", "") != "removed" && r.contains("identity")) {
      if (action == "quiesce") {
        try {
          qemu::command(run, r, action);
        } catch (...) {
          auto adopted = qemu::command(run, r, "adopt");
          (void)adopted;
          qemu::command(run, r, action);
        }
      } else
        qemu::command(run, r, action);
    }
  for (const auto &r : run["resources"])
    if (r["kind"] == "container" && r.value("state", "") != "removed") {
      bool leased = run["topology"]["capture"]["required"] == true;
      if (action == "renew" && !leased)
        continue;
      auto operation = leased && action == "release" ? "release-lease"
                       : action == "renew"           ? "renew-lease"
                                                     : action;
      auto result = node_exec(run, r, operation);
      if (action == "release" || action == "renew") {
        auto version = lab_support::gate_protocol_minor(result);
        if (!version)
          throw Failure(version.error().code);
      }
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
      if (r["kind"] == "qemu" && r.contains("identity"))
        nodes.push_back({{"id", r["logical"]}, {"guest", qemu::command(run, r, "status")}});
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
namespace {
Json edge_resource(const Json &run, const std::string &id) {
  for (const auto &r : run["resources"])
    if (r["kind"] == "edge" && r["logical"] == id && r.value("state", "") != "removed")
      return r;
  throw Failure("edge_not_available", 404);
}
Json endpoint_call(const Json &run, const Json &r, int index,
                   const std::function<Json(const std::vector<std::string> &, const Json &)> &fn,
                   bool missing = false) {
  if (!r.contains("identity"))
    throw Failure("edge_mapping_unavailable");
  auto expected = r["identity"]["endpoints"][index];
  auto name = expected["name"].get<std::string>();
  auto check = [&](const std::vector<std::string> &prefix) {
    auto found = link(name, prefix);
    if (found.is_null()) {
      if (missing)
        return Json{{"absent", true}};
      throw Failure("edge_mapping_missing");
    }
    check_link(found, owner(run, r), expected);
    return fn(prefix, found);
  };
  if (expected.value("namespace", "") == "host")
    return check({});
  auto ep = r["configuration"]["endpoints"][index].get<std::string>();
  auto nr = node_resource(run, ep.substr(0, ep.find(':')));
  auto container = inspect_container(run, nr);
  Namespace ns(container, expected);
  return check(ns_prefix(ns));
}
Json tc_show(const std::vector<std::string> &prefix, const std::string &name) {
  auto a = prefix;
  a.insert(a.end(), {"/usr/sbin/tc", "-j", "-s", "qdisc", "show", "dev", name});
  return Json::parse(command(a));
}
void tc(const std::vector<std::string> &prefix, std::vector<std::string> tail) {
  auto a = prefix;
  a.push_back("/usr/sbin/tc");
  a.insert(a.end(), tail.begin(), tail.end());
  command(a);
}
void stringify_numbers(Json &v) {
  if (v.is_number_integer() || v.is_number_unsigned())
    v = v.dump();
  else if (v.is_structured())
    for (auto &i : v)
      stringify_numbers(i);
}
} // namespace
Json LinuxBackend::telemetry(const Json &run) {
  Json out = Json::array(), cache = Json::object(), times = Json::object();
  if (graphlab::telemetry::monotonic() - tree_time_ >= 2000000000ull) {
    tree_cache_ = Json::object();
    try {
      auto rows = Json::parse(command({"/usr/bin/ovs-vsctl", "--timeout=2", "--format=json",
                                       "--columns=name,rstp_status", "list", "Port"}));
      for (const auto &row : rows["data"]) {
        Json value = Json::object();
        if (row[1].is_array() && row[1].size() == 2 && row[1][0] == "map")
          for (const auto &pair : row[1][1])
            value[pair[0].get<std::string>()] = pair[1];
        tree_cache_[row[0].get<std::string>()] = value;
      }
    } catch (...) {
      tree_cache_ = Json::object();
    }
    tree_time_ = graphlab::telemetry::monotonic();
  }
  for (const auto &r : run["resources"])
    if (r["kind"] == "edge" && r.value("state", "") != "removed") {
      Json s = {{"edge", r["logical"]}, {"valid", false}, {"source", "rtnetlink/iproute2-stats64"}};
      try {
        auto endpoints = r["identity"]["endpoints"];
        int index = endpoints[0].value("namespace", "") == "host"   ? 0
                    : endpoints[1].value("namespace", "") == "host" ? 1
                                                                    : 0;
        auto point = endpoints[index];
        bool tap = point.value("kind", "") == "tap";
        auto ep0 = r["configuration"]["endpoints"][0].get<std::string>();
        bool forward_rx =
            tap ? node_resource(run, ep0.substr(0, ep0.find(':')))["kind"] == "qemu" : index == 1;
        s["forwardMetric"] = forward_rx ? "rx" : "tx";
        s["endpoints"] = r["configuration"]["endpoints"];
        s["canonicalEndpoint"] = index;
        s["mapping"] = point;
        s["mappingEpoch"] = lab_support::digest(point);
        endpoint_call(run, r, index, [&](const auto &prefix, const Json &found) {
          auto key = point.value("namespaceInode", std::string("host"));
          if (!cache.contains(key)) {
            auto a = prefix;
            a.insert(a.end(), {"/usr/sbin/ip", "-j", "-s", "-s", "link", "show"});
            cache[key] = Json::parse(command(a));
            times[key] = std::to_string(graphlab::telemetry::monotonic());
          }
          s["monotonicNs"] = times[key];
          Json counters;
          for (const auto &v : cache[key])
            if (v["ifindex"] == found["ifindex"])
              counters = v;
          if (!counters.contains("stats64"))
            throw Failure("stats64_unavailable");
          s["raw"] = Json::object();
          for (auto dir : {"rx", "tx"})
            for (auto field : {"bytes", "packets", "errors", "dropped"}) {
              std::string suffix = field;
              suffix[0] = std::toupper(suffix[0]);
              auto value = counters["stats64"][dir][field];
              if (!value.is_number_unsigned() && !value.is_number_integer())
                throw Failure("counter_unavailable");
              s["raw"][std::string(dir) + suffix] = value.dump();
            }
          s["adminUp"] =
              std::find(found["flags"].begin(), found["flags"].end(), "UP") != found["flags"].end();
          s["carrierUp"] = std::find(found["flags"].begin(), found["flags"].end(), "LOWER_UP") !=
                           found["flags"].end();
          s["operstate"] = found.value("operstate", "UNKNOWN");
          s["qdiscs"] = tc_show(prefix, point["name"]);
          stringify_numbers(s["qdiscs"]);
          return Json::object();
        });
        s["valid"] = true;
        s["rstp"] = Json::object();
        for (const auto &p : endpoints)
          if (tree_cache_.contains(p["name"].get<std::string>()))
            s["rstp"][p["name"].get<std::string>()] = tree_cache_[p["name"].get<std::string>()];
        s["rstpObservedMonotonicNs"] = std::to_string(tree_time_);
      } catch (const std::exception &e) {
        s["reason"] = e.what();
      }
      out.push_back(s);
    }
  return out;
}
Json LinuxBackend::fault(const Json &run, const Json &f, const std::string &action) {
  auto r = edge_resource(run, f["edge"]);
  int index = f["direction"] == "a-to-b" ? 0 : 1;
  auto ep = r["configuration"]["endpoints"][index].get<std::string>();
  auto source = node_resource(run, ep.substr(0, ep.find(':')));
  // Direct links can shape delivery on the opposite host attachment egress.
  // Shared switch-to-guest TAPs still need IFB for the reverse direction.
  if (source["kind"] == "qemu") {
    if (direct_guest(run, r)) {
      index = 1 - index;
      r["identity"]["endpoints"][index] = r["identity"]["attachment"]["ports"][index];
    } else
      throw Failure("guest_egress_fault_requires_ifb");
  }
  auto point = r["identity"]["endpoints"][index];
  auto name = point["name"].get<std::string>();
  auto h = lab_support::digest(Json::array({run["id"], f["id"]}));
  auto number = std::stoul(h.substr(7, 4), nullptr, 16);
  number = 0x1000 + (number % 0xd000);
  std::ostringstream hex;
  hex << std::hex << number;
  std::string handle = hex.str() + ":";
  Json placement = {{"edge", f["edge"]},    {"direction", f["direction"]},
                    {"sourceEndpoint", ep}, {"endpointIndex", index},
                    {"mapping", point},     {"mappingEpoch", lab_support::digest(point)},
                    {"hook", "egress"},     {"handle", handle}};
  if (action != "plan" && f.at("placement") != placement)
    throw Failure("fault_mapping_changed", 409);
  return endpoint_call(
      run, r, index,
      [&](const auto &prefix, const Json &) {
        auto q = tc_show(prefix, name);
        bool ours = false, parent_seen = false, neutral_seen = false;
        bool child = point.contains("faultParent");
        for (const auto &v : q) {
          if (child) {
            if (v.value("root", false)) {
              if (v.value("kind", "") != "prio" || v["handle"] != point["faultRoot"] ||
                  v["options"]["bands"] != 3 ||
                  v["options"]["priomap"] !=
                      Json::array({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}))
                throw Failure("foreign_root_qdisc", 409);
              parent_seen = true;
            } else if (v.value("handle", "") == handle && v.value("kind", "") == "netem" &&
                       v["parent"] == point["faultParent"])
              ours = true;
            else if (v.value("kind", "") == "pfifo" && v["handle"] == point["faultNeutral"] &&
                     v["parent"] == point["faultParent"] && v["options"]["limit"] == 1000)
              neutral_seen = true;
            else
              throw Failure("foreign_child_qdisc", 409);
            continue;
          }
          if (v.value("root", false) && v.value("kind", "") != "noqueue") {
            if (v.value("handle", "") == handle && v.value("kind", "") == "netem")
              ours = true;
            else
              throw Failure("foreign_root_qdisc", 409);
          }
        }
        if (child && (!parent_seen || (!ours && !neutral_seen)))
          throw Failure("fault_parent_missing");
        if (action == "plan") {
          if (ours)
            throw Failure("qdisc_already_exists", 409);
          return placement;
        }
        if ((action == "apply" && !ours) || (action == "remove" && ours)) {
          std::vector<std::string> args = {"qdisc",
                                           child               ? "replace"
                                           : action == "apply" ? "add"
                                                               : "del",
                                           "dev", name};
          if (child)
            args.insert(args.end(), {"parent", point["faultParent"]});
          else
            args.push_back("root");
          args.insert(args.end(), {"handle", child && action == "remove"
                                                 ? point["faultNeutral"].get<std::string>()
                                                 : handle});
          if (child && action == "remove")
            args.insert(args.end(), {"pfifo", "limit", "1000"});
          if (action == "apply")
            args.insert(args.end(), {"netem", "limit", "1000", "delay",
                                     std::to_string(f["delayMs"].get<int>()) + "ms", "loss",
                                     std::to_string(f["lossPercent"].get<int>()) + "%"});
          tc(prefix, args);
        }
        detail::checkpoint("fault." + action + ".created");
        auto observed = tc_show(prefix, name);
        bool present = false, restored = false;
        for (const auto &v : observed) {
          if (v.value("handle", "") == handle && v.value("kind", "") == "netem")
            present = true;
          if (child && v.value("kind", "") == "pfifo" && v["handle"] == point["faultNeutral"] &&
              v["parent"] == point["faultParent"] && v["options"]["limit"] == 1000)
            restored = true;
        }
        if ((action == "apply") != present || (child && action == "remove" && !restored))
          throw Failure("fault_readback_failed");
        return Json{{"placement", placement},
                    {"active", present},
                    {"qdiscs", observed},
                    {"observedAt", console::timestamp()}};
      },
      action == "remove");
}

} // namespace graphlab::runtime
