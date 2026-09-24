#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <lab_support/contracts.hpp>
#include <regex>
#include <set>
#include <stdexcept>

namespace lab_support {
namespace {
[[noreturn]] void fail(std::string path, std::string message,
                       std::string code = "validation_error") {
  throw Error{std::move(code), std::move(path), std::move(message)};
}
void object(const Json &v, const std::string &p) {
  if (!v.is_object())
    fail(p, "expected mapping");
}
void fields(const Json &v, const std::string &p, std::initializer_list<std::string_view> allowed) {
  object(v, p);
  for (auto it = v.begin(); it != v.end(); ++it)
    if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
      fail(p + "." + it.key(), "unknown field");
}
const Json &need(const Json &v, std::string_view k, std::string_view p) {
  if (!v.contains(k))
    fail(std::string(p) + "." + std::string(k), "required field missing");
  return v.at(k);
}
std::string str(const Json &v, const std::string &p) {
  if (!v.is_string())
    fail(p, "expected string");
  return v.get<std::string>();
}
std::string getstr(const Json &v, const std::string &k, const std::string &p) {
  return str(need(v, k, p), p + "." + k);
}
bool boolean(const Json &v, const std::string &p) {
  if (!v.is_boolean())
    fail(p, "expected boolean");
  return v.get<bool>();
}
int number(const Json &v, const std::string &p, int lo, int hi) {
  if (!v.is_number_integer() || v < lo || v > hi)
    fail(p, "integer outside [" + std::to_string(lo) + "," + std::to_string(hi) + "]");
  return v.get<int>();
}
void array(const Json &v, const std::string &p) {
  if (!v.is_array())
    fail(p, "expected sequence");
}
void id(const std::string &s, const std::string &p) {
  static const std::regex pattern("[A-Za-z][A-Za-z0-9_-]{0,63}");
  if (!std::regex_match(s, pattern))
    fail(p, "identifier must match [A-Za-z][A-Za-z0-9_-]{0,63}");
}
void hash(const std::string &s, const std::string &p) {
  static const std::regex pattern("sha256:[0-9a-f]{64}");
  if (!std::regex_match(s, pattern))
    fail(p, "expected resolved lowercase SHA-256 digest");
}
void features(const Json &v, const std::string &p) {
  if (!v.contains("requiredFeatures"))
    return;
  array(v["requiredFeatures"], p + ".requiredFeatures");
  if (!v["requiredFeatures"].empty())
    fail(p + ".requiredFeatures", "no optional required features supported in M0",
         "unsupported_feature");
}
struct Cidr {
  std::uint32_t ip, mask;
  int prefix;
};
Cidr cidr(const std::string &text, const std::string &p) {
  auto slash = text.find('/');
  if (slash == std::string::npos)
    fail(p, "IPv4 CIDR required");
  int prefix = -1;
  auto [ptr, ec] = std::from_chars(text.data() + slash + 1, text.data() + text.size(), prefix);
  in_addr addr{};
  if (ec != std::errc{} || ptr != text.data() + text.size() || prefix < 0 || prefix > 32 ||
      inet_pton(AF_INET, text.substr(0, slash).c_str(), &addr) != 1)
    fail(p, "invalid IPv4 CIDR");
  return {ntohl(addr.s_addr), prefix == 0 ? 0u : 0xffffffffu << (32 - prefix), prefix};
}
bool inside(Cidr a, Cidr b) { return (a.ip & b.mask) == (b.ip & b.mask); }
Json workload(const Json &c) {
  const std::string p = "workload";
  fields(c, p,
         {"apiVersion", "id", "kind", "platforms", "interfaces", "lifecycle", "labSupport",
          "resources", "requiredFeatures"});
  if (getstr(c, "apiVersion", p) != "graphlab.workload/v2")
    fail(p + ".apiVersion", "unsupported workload major", "unsupported_version");
  id(getstr(c, "id", p), p + ".id");
  const auto kind = getstr(c, "kind", p);
  if (kind != "docker" && kind != "qemu")
    fail(p + ".kind", "expected docker or qemu");
  features(c, p);
  const auto &platforms = need(c, "platforms", p);
  array(platforms, p + ".platforms");
  if (platforms.empty())
    fail(p + ".platforms", "at least one platform required");
  std::set<std::string> seen;
  for (const auto &v : platforms) {
    auto s = str(v, p + ".platforms");
    if (s != "linux/amd64" && s != "linux/arm64" && !(kind == "qemu" && s == "linux/ppc64le"))
      fail(p + ".platforms", "unsupported platform");
    if (!seen.insert(s).second)
      fail(p + ".platforms", "duplicate platform");
  }
  const auto &interfaces = need(c, "interfaces", p);
  object(interfaces, p + ".interfaces");
  for (const auto &[name, v] : interfaces.items()) {
    auto q = p + ".interfaces." + name;
    id(name, q);
    fields(v, q, {"role", "required", "medium", "mtuRange"});
    auto role = getstr(v, "role", q);
    if (role != "data" && role != "management")
      fail(q + ".role", "unsupported interface role");
    boolean(need(v, "required", q), q + ".required");
    if (getstr(v, "medium", q) != "ethernet")
      fail(q + ".medium", "only ethernet supported");
    const auto &r = need(v, "mtuRange", q);
    array(r, q + ".mtuRange");
    if (r.size() != 2)
      fail(q + ".mtuRange", "requires lower and upper bounds");
    int lo = number(r[0], q, 576, 9216), hi = number(r[1], q, 576, 9216);
    if (lo > hi)
      fail(q + ".mtuRange", "reversed range");
  }
  auto life = need(c, "lifecycle", p);
  fields(life, p + ".lifecycle", {"gateUntilRelease", "quiesce"});
  if (!boolean(need(life, "gateUntilRelease", p), p + ".lifecycle.gateUntilRelease"))
    fail(p + ".lifecycle", "M0 plans require a startup gate");
  auto qmode = getstr(life, "quiesce", p + ".lifecycle");
  if (qmode != "supported" && qmode != "restart-required")
    fail(p + ".lifecycle.quiesce", "unknown quiescence mode");
  const auto &support = need(c, "labSupport", p);
  fields(support, p + ".labSupport", {"language", "version", "packageSha256"});
  if (getstr(support, "language", p) != "cpp23")
    fail(p + ".labSupport.language", "custom lab support must use cpp23");
  static const std::regex semver("[0-9]+\\.[0-9]+\\.[0-9]+");
  if (!std::regex_match(getstr(support, "version", p), semver))
    fail(p + ".labSupport.version", "resolved semantic version required");
  hash(getstr(support, "packageSha256", p), p + ".labSupport.packageSha256");
  const auto &resources = need(c, "resources", p);
  fields(resources, p + ".resources", {"cpus", "memoryMiB"});
  number(need(resources, "cpus", p), p + ".resources.cpus", 1, 1024);
  number(need(resources, "memoryMiB", p), p + ".resources.memoryMiB", 16, 1048576);
  Json result = c;
  result["requiredFeatures"] = Json::array();
  std::sort(result["platforms"].begin(), result["platforms"].end());
  return result;
}
void validate_lock(const Json &lock) {
  fields(lock, "lock", {"apiVersion", "workloads"});
  if (getstr(lock, "apiVersion", "lock") != "graphlab.artifacts/v1")
    fail("lock.apiVersion", "unsupported artifact lock", "unsupported_version");
  const auto &entries = need(lock, "workloads", "lock");
  object(entries, "lock.workloads");
  for (const auto &[key, e] : entries.items()) {
    std::string p = "lock.workloads." + key;
    id(key, p);
    fields(e, p, {"contract", "contractSha256", "image", "diskSha256", "platform", "vm"});
    auto raw = need(e, "contract", p);
    auto c = workload(raw);
    if (c["id"] != key)
      fail(p + ".contract.id", "contract ID differs from lock key");
    auto h = getstr(e, "contractSha256", p);
    hash(h, p + ".contractSha256");
    if (digest(raw) != h)
      fail(p + ".contractSha256", "embedded contract hash mismatch", "integrity_error");
    auto platform = getstr(e, "platform", p);
    if (std::find(c["platforms"].begin(), c["platforms"].end(), platform) == c["platforms"].end())
      fail(p + ".platform", "not supported by contract");
    if (c["kind"] == "docker") {
      auto image = getstr(e, "image", p);
      auto at = image.find('@');
      if (at == std::string::npos || at == 0 || image.find_first_of(" \t\n") != std::string::npos)
        fail(p + ".image", "immutable name@sha256 image required");
      hash(image.substr(at + 1), p + ".image");
      if (e.contains("diskSha256") || e.contains("vm"))
        fail(p, "Docker artifact cannot have VM fields");
    } else {
      if (e.contains("image"))
        fail(p, "QEMU artifact cannot have container image");
      hash(getstr(e, "diskSha256", p), p + ".diskSha256");
      const auto &vm = need(e, "vm", p);
      fields(vm, p + ".vm",
             {"machine", "firmwareSha256", "accelerator", "kernelSha256", "initrdSha256", "sshUser",
              "knownHostsSha256", "runnerImage"});
      if (vm.contains("runnerImage"))
        hash(getstr(vm, "runnerImage", p), p + ".vm.runnerImage");
      if (vm.contains("sshUser") || vm.contains("knownHostsSha256")) {
        id(getstr(vm, "sshUser", p), p + ".vm.sshUser");
        hash(getstr(vm, "knownHostsSha256", p), p + ".vm.knownHostsSha256");
      }
      if (vm.contains("kernelSha256") || vm.contains("initrdSha256")) {
        hash(getstr(vm, "kernelSha256", p), p + ".vm.kernelSha256");
        hash(getstr(vm, "initrdSha256", p), p + ".vm.initrdSha256");
      }
      static const std::regex machine("[A-Za-z0-9_-]+-[0-9]+\\.[0-9]+(\\.[0-9]+)?");
      if (!std::regex_match(getstr(vm, "machine", p), machine))
        fail(p + ".vm.machine", "versioned QEMU machine required");
      hash(getstr(vm, "firmwareSha256", p), p + ".vm.firmwareSha256");
      if (getstr(vm, "accelerator", p) != "kvm" && getstr(vm, "accelerator", p) != "tcg")
        fail(p + ".vm.accelerator", "explicit kvm or tcg accelerator required");
    }
  }
}
} // namespace
Result<Json> validate_workload(const Json &c) {
  try {
    return workload(c);
  } catch (const Error &e) {
    return std::unexpected(e);
  } catch (const std::exception &e) {
    return std::unexpected(Error{"validation_error", "workload", e.what()});
  }
}
Result<ValidatedTopology> validate(const Json &t, const Json &lock) {
  try {
    validate_lock(lock);
    fields(t, "topology",
           {"apiVersion", "id", "artifactLock", "backend", "management", "nodes", "edges",
            "managementAttachments", "capture", "requiredFeatures", "limits", "loopPolicy",
            "application"});
    if (getstr(t, "apiVersion", "topology") != "graphlab.topology/v2")
      fail("topology.apiVersion", "unsupported topology major", "unsupported_version");
    id(getstr(t, "id", "topology"), "topology.id");
    features(t, "topology");
    auto lockhash = getstr(t, "artifactLock", "topology");
    hash(lockhash, "topology.artifactLock");
    if (digest(lock) != lockhash)
      fail("topology.artifactLock", "artifact lock hash mismatch", "integrity_error");
    const auto &backend = need(t, "backend", "topology");
    fields(backend, "backend", {"kind", "switchIsolation"});
    if (getstr(backend, "kind", "backend") != "linux-local" ||
        getstr(backend, "switchIsolation", "backend") != "shared-ovs")
      fail("backend", "only linux-local/shared-ovs is supported", "unsupported_backend");
    auto capture = need(t, "capture", "topology");
    fields(capture, "capture", {"required", "scope", "format"});
    boolean(need(capture, "required", "capture"), "capture.required");
    if (getstr(capture, "scope", "capture") != "all-data-edges" ||
        getstr(capture, "format", "capture") != "pcapng")
      fail("capture", "expected all-data-edges / pcapng");
    int maxnodes = 1024, maxedges = 8192;
    if (t.contains("limits")) {
      fields(t["limits"], "limits", {"maxNodes", "maxEdges"});
      maxnodes = number(need(t["limits"], "maxNodes", "limits"), "limits.maxNodes", 1, 10000);
      maxedges = number(need(t["limits"], "maxEdges", "limits"), "limits.maxEdges", 0, 100000);
    }
    const auto loop = t.contains("loopPolicy") ? str(t["loopPolicy"], "loopPolicy") : "rstp";
    if (loop != "rstp" && loop != "unprotected")
      fail("loopPolicy", "expected rstp or explicit unprotected");
    ValidatedTopology out;
    out.canonical = t;
    out.artifacts = lock;
    out.canonical["limits"] = {{"maxNodes", maxnodes}, {"maxEdges", maxedges}};
    out.canonical["loopPolicy"] = loop;
    out.canonical["requiredFeatures"] = Json::array();
    const auto &nodes = need(t, "nodes", "topology");
    object(nodes, "nodes");
    if (nodes.size() > static_cast<std::size_t>(maxnodes))
      fail("nodes", "node budget exceeded", "resource_limit");
    for (const auto &[key, n] : nodes.items()) {
      std::string p = "nodes." + key;
      id(key, p);
      fields(n, p, {"kind", "workload", "ports", "policy", "addresses"});
      auto kind = getstr(n, "kind", p);
      if (kind != "docker" && kind != "qemu" && kind != "ovs-switch")
        fail(p + ".kind", "unsupported node kind", "unsupported_backend");
      Node node{kind, {}, {}};
      if (kind == "ovs-switch") {
        if (n.contains("workload") || n.contains("addresses"))
          fail(p, "switch cannot carry workload or host addresses");
        const auto &policy = need(n, "policy", p);
        fields(policy, p + ".policy", {"forwarding", "rstp", "priority"});
        if (getstr(policy, "forwarding", p) != "normal")
          fail(p + ".policy.forwarding", "only normal forwarding supported");
        boolean(need(policy, "rstp", p), p + ".policy.rstp");
        int priority = number(need(policy, "priority", p), p + ".policy.priority", 0, 61440);
        if (priority % 4096)
          fail(p + ".policy.priority", "RSTP priority must be multiple of 4096");
      } else {
        if (n.contains("policy"))
          fail(p + ".policy", "switch policy on a workload");
        node.workload = getstr(n, "workload", p);
        if (!lock["workloads"].contains(node.workload))
          fail(p + ".workload", "unresolved workload reference");
        if (lock["workloads"][node.workload]["contract"]["kind"] != kind)
          fail(p + ".kind", "workload kind mismatch");
      }
      const auto &ports = need(n, "ports", p);
      object(ports, p + ".ports");
      for (const auto &[name, v] : ports.items()) {
        auto q = p + ".ports." + name;
        id(name, q);
        fields(v, q, {"role", "medium", "mtu", "vlan"});
        auto role = getstr(v, "role", q);
        if (role != "data" && role != "management")
          fail(q + ".role", "unknown role");
        if (kind == "ovs-switch" && role != "data")
          fail(q + ".role", "switch management uses host socket, not a data bridge port");
        auto medium = v.contains("medium") ? str(v["medium"], q + ".medium") : "ethernet";
        if (medium != "ethernet")
          fail(q + ".medium", "only ethernet supported");
        int mtu = v.contains("mtu") ? number(v["mtu"], q + ".mtu", 576, 9216) : 1500;
        Json vlan = Json::object();
        if (v.contains("vlan")) {
          if (kind != "ovs-switch")
            fail(q + ".vlan", "VLAN wiring belongs to switch ports");
          vlan = v["vlan"];
          fields(vlan, q + ".vlan", {"access", "trunk"});
          if (vlan.size() != 1)
            fail(q + ".vlan", "exactly one access or trunk policy required");
          if (vlan.contains("access"))
            number(vlan["access"], q + ".vlan.access", 1, 4094);
          else {
            array(vlan["trunk"], q + ".vlan.trunk");
            std::set<int> ids;
            for (const auto &value : vlan["trunk"])
              if (!ids.insert(number(value, q + ".vlan.trunk", 1, 4094)).second)
                fail(q + ".vlan.trunk", "duplicate VLAN");
            if (ids.empty())
              fail(q + ".vlan.trunk", "explicit nonempty trunk required");
            std::sort(vlan["trunk"].begin(), vlan["trunk"].end());
          }
        } else if (kind == "ovs-switch")
          fail(q + ".vlan", "explicit switch VLAN policy required");
        if (kind != "ovs-switch") {
          auto &interfaces = lock["workloads"][node.workload]["contract"]["interfaces"];
          if (!interfaces.contains(name))
            fail(q, "port not declared by workload");
          auto &spec = interfaces[name];
          if (spec["role"] != role || spec["medium"] != medium || mtu < spec["mtuRange"][0] ||
              mtu > spec["mtuRange"][1])
            fail(q, "port incompatible with workload contract");
        }
        node.ports.emplace(name, Port{role, medium, mtu, vlan});
        auto &canonical = out.canonical["nodes"][key]["ports"][name];
        canonical["medium"] = medium;
        canonical["mtu"] = mtu;
        if (!vlan.empty())
          canonical["vlan"] = vlan;
      }
      if (kind != "ovs-switch") {
        for (const auto &[name, v] :
             lock["workloads"][node.workload]["contract"]["interfaces"].items())
          if (v["required"] == true && !node.ports.contains(name))
            fail(p + ".ports." + name, "required workload interface missing");
      }
      if (n.contains("addresses")) {
        object(n["addresses"], p + ".addresses");
        for (const auto &[name, v] : n["addresses"].items()) {
          if (!node.ports.contains(name) || node.ports.at(name).role != "data")
            fail(p + ".addresses." + name, "address requires declared data port");
          cidr(str(v, p + ".addresses." + name), p + ".addresses." + name);
        }
      }
      out.nodes.emplace(key, std::move(node));
    }
    auto endpoint = [&](const std::string &e,
                        const std::string &p) -> std::pair<std::string, const Port *> {
      auto colon = e.find(':');
      if (colon == std::string::npos || e.find(':', colon + 1) != std::string::npos)
        fail(p, "endpoint must be node:port");
      auto node = e.substr(0, colon), port = e.substr(colon + 1);
      if (!out.nodes.contains(node) || !out.nodes.at(node).ports.contains(port))
        fail(p, "undeclared endpoint: " + e);
      return {node, &out.nodes.at(node).ports.at(port)};
    };
    const auto &edges = need(t, "edges", "topology");
    array(edges, "edges");
    if (edges.size() > static_cast<std::size_t>(maxedges))
      fail("edges", "edge budget exceeded", "resource_limit");
    std::set<std::string> edge_ids, used;
    for (const auto &e : edges) {
      fields(e, "edge", {"id", "endpoints"});
      auto key = getstr(e, "id", "edge");
      id(key, "edge.id");
      if (!edge_ids.insert(key).second)
        fail("edges." + key, "duplicate edge ID");
      auto p = "edges." + key;
      const auto &eps = need(e, "endpoints", p);
      array(eps, p + ".endpoints");
      if (eps.size() != 2)
        fail(p, "exactly two endpoints required");
      auto first = str(eps[0], p), second = str(eps[1], p);
      auto [a, ap] = endpoint(first, p);
      auto [b, bp] = endpoint(second, p);
      if (!used.insert(first).second || !used.insert(second).second)
        fail(p, "endpoint reused");
      if (ap->role != "data" || bp->role != "data" || ap->medium != bp->medium ||
          ap->mtu != bp->mtu)
        fail(p, "incompatible endpoint roles, media or MTUs");
      const auto &ak = out.nodes.at(a).kind;
      // Direct guest edges use a scoped attachment bridge, not a modeled switch.
      if (a == b && (ak != "ovs-switch" || loop != "unprotected"))
        fail(p, "self-links require switch ports and explicit unprotected loopPolicy");
      out.edges.push_back({key, first, second});
    }
    if (t.contains("application")) {
      const auto &app = t["application"];
      fields(app, "application", {"apiVersion", "edges"});
      if (getstr(app, "apiVersion", "application") != "graphlab.application-dataflow/v1")
        fail("application.apiVersion", "unsupported dataflow version", "unsupported_version");
      const auto &flows = need(app, "edges", "application");
      array(flows, "application.edges");
      if (flows.size() > 1024)
        fail("application.edges", "application edge budget exceeded", "resource_limit");
      std::set<std::string> ids;
      Json normalized = Json::array();
      for (const auto &flow : flows) {
        fields(flow, "application.edge", {"id", "source", "target", "networkEdges", "protocol"});
        auto key = getstr(flow, "id", "application.edge");
        id(key, "application.edge.id");
        auto p = "application.edges." + key;
        if (!ids.insert(key).second)
          fail(p, "duplicate application edge ID");
        for (auto field : {"source", "target"}) {
          auto node = getstr(flow, field, p);
          if (!out.nodes.contains(node) || out.nodes.at(node).kind == "ovs-switch")
            fail(p + "." + field, "expected declared workload node");
        }
        if (flow.contains("protocol")) {
          const auto &protocol = flow["protocol"];
          fields(protocol, p + ".protocol", {"apiVersion", "transport", "framing", "schema"});
          if (getstr(protocol, "apiVersion", p) != "graphlab.application-protocol/v1")
            fail(p + ".protocol.apiVersion", "unsupported protocol declaration version");
          for (auto field : {"transport", "framing", "schema"}) {
            auto value = getstr(protocol, field, p);
            if (!std::regex_match(value, std::regex("[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}")))
              fail(p + ".protocol", "invalid or oversized declaration identifier");
          }
        }
        const auto &refs = need(flow, "networkEdges", p);
        array(refs, p + ".networkEdges");
        if (refs.size() > 256)
          fail(p, "network mapping budget exceeded", "resource_limit");
        std::set<std::string> links;
        for (const auto &ref : refs) {
          auto link = str(ref, p + ".networkEdges");
          if (!edge_ids.contains(link) || !links.insert(link).second)
            fail(p + ".networkEdges", "unknown or duplicate data edge");
        }
        auto value = flow;
        value["networkEdges"] = links;
        normalized.push_back(value);
      }
      std::sort(normalized.begin(), normalized.end(),
                [](const Json &a, const Json &b) { return a["id"] < b["id"]; });
      out.canonical["application"]["edges"] = normalized;
    }
    // For the normal switching profile, every switch in a cyclic component must enable RSTP.
    std::map<std::string, std::string> parent;
    for (const auto &[key, n] : out.nodes) {
      (void)n;
      parent[key] = key;
    }
    auto root = [&](std::string x) {
      while (parent[x] != x)
        x = parent[x];
      return x;
    };
    std::set<std::string> cyclic_nodes;
    for (const auto &e : out.edges) {
      auto a = e.first.substr(0, e.first.find(':')), b = e.second.substr(0, e.second.find(':'));
      if (out.nodes.at(a).kind != "ovs-switch" || out.nodes.at(b).kind != "ovs-switch")
        continue;
      auto ra = root(a), rb = root(b);
      if (ra == rb)
        cyclic_nodes.insert(a);
      else
        parent[ra] = rb;
    }
    if (loop == "rstp")
      for (const auto &[key, n] : out.nodes)
        if (n.kind == "ovs-switch")
          for (const auto &cyc : cyclic_nodes)
            if (root(key) == root(cyc) && t["nodes"][key]["policy"]["rstp"] != true)
              fail("nodes." + key + ".policy.rstp",
                   "cyclic switch component needs RSTP or explicit unprotected policy");
    const auto &mgmt = need(t, "management", "topology");
    fields(mgmt, "management", {"networks"});
    const auto &nets = need(mgmt, "networks", "management");
    object(nets, "management.networks");
    std::map<std::string, Cidr> subnets, pools;
    std::map<std::string, std::set<std::uint32_t>> allocations;
    for (const auto &[name, n] : nets.items()) {
      auto p = "management.networks." + name;
      id(name, p);
      fields(n, p, {"subnet", "dynamicPool", "gateway", "externalAccess"});
      auto subnet = cidr(getstr(n, "subnet", p), p + ".subnet"),
           pool = cidr(getstr(n, "dynamicPool", p), p + ".dynamicPool");
      if (subnet.prefix < 8 || subnet.prefix > 30 || subnet.ip != (subnet.ip & subnet.mask))
        fail(p + ".subnet", "canonical IPv4 network with prefix 8..30 required");
      if (pool.prefix < subnet.prefix || pool.ip != (pool.ip & pool.mask) || !inside(pool, subnet))
        fail(p + ".dynamicPool", "canonical pool must be contained by subnet");
      for (const auto &[other, s] : subnets)
        if (inside(subnet, s) || inside(s, subnet))
          fail(p, "management networks overlap: " + other);
      auto gw = cidr(getstr(n, "gateway", p) + "/" + std::to_string(subnet.prefix), p + ".gateway");
      if (!inside(gw, subnet) || inside(gw, pool) || gw.ip == subnet.ip ||
          gw.ip == (subnet.ip | ~subnet.mask))
        fail(p + ".gateway", "gateway must be usable and outside dynamic pool");
      if (boolean(need(n, "externalAccess", p), p + ".externalAccess"))
        fail(p + ".externalAccess", "external management routing is not supported in M0");
      subnets[name] = subnet;
      pools[name] = pool;
      allocations[name].insert(gw.ip);
    }
    auto attachments = t.value("managementAttachments", Json::array());
    array(attachments, "managementAttachments");
    for (const auto &a : attachments) {
      fields(a, "attachment", {"endpoint", "network", "address"});
      auto ep = getstr(a, "endpoint", "attachment");
      auto [key, port] = endpoint(ep, "attachment.endpoint");
      (void)key;
      if (port->role != "management" || !used.insert(ep).second)
        fail("attachment.endpoint", "management port required and may attach only once");
      auto network = getstr(a, "network", "attachment");
      if (!subnets.contains(network))
        fail("attachment.network", "unknown management network");
      auto address = cidr(getstr(a, "address", "attachment"), "attachment.address");
      auto net = subnets.at(network);
      if (address.prefix != net.prefix || !inside(address, net) ||
          inside(address, pools.at(network)) || address.ip == net.ip ||
          address.ip == (net.ip | ~net.mask) || !allocations[network].insert(address.ip).second)
        fail("attachment.address",
             "address must be unique, usable, in subnet and outside dynamic pool");
    }
    std::sort(out.edges.begin(), out.edges.end(),
              [](const Edge &a, const Edge &b) { return a.id < b.id; });
    std::sort(attachments.begin(), attachments.end(),
              [](const Json &a, const Json &b) { return a["endpoint"] < b["endpoint"]; });
    out.canonical["managementAttachments"] = attachments;
    out.canonical["edges"] = Json::array();
    for (const auto &e : out.edges)
      out.canonical["edges"].push_back(
          {{"id", e.id}, {"endpoints", Json::array({e.first, e.second})}});
    out.hash = digest(out.canonical);
    return out;
  } catch (const Error &e) {
    return std::unexpected(e);
  } catch (const std::exception &e) {
    return std::unexpected(Error{"validation_error", "$", e.what()});
  }
}
} // namespace lab_support
