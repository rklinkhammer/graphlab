#include <algorithm>
#include <graphlab/planner.hpp>
#include <map>
#include <set>

namespace graphlab {
using namespace lab_support;
Result<Json> plan(const Json &topology, const Json &artifacts) {
  auto validated = validate(topology, artifacts);
  if (!validated)
    return std::unexpected(validated.error());
  const auto &t = *validated;
  std::map<std::string, Json> steps;
  auto add = [&](std::string key, std::string operation, std::vector<std::string> dependencies,
                 Json resource = Json::object()) {
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    steps[key] = {
        {"id", key}, {"operation", operation}, {"dependsOn", dependencies}, {"resource", resource}};
  };
  add("preflight", "verify-runtime-and-reserve", {},
      {{"executionAllowed", false}, {"runtimeChecks", "deferred"}});
  std::vector<std::string> preparation;
  for (const auto &[id, network] : t.canonical["management"]["networks"].items()) {
    auto key = "management/" + id;
    add(key, "prepare-management", {"preflight"}, {{"network", id}, {"configuration", network}});
    preparation.push_back(key);
  }
  for (const auto &[id, node] : t.nodes) {
    std::vector<std::string> deps{"preflight"};
    for (const auto &a : t.canonical["managementAttachments"])
      if (a["endpoint"].get<std::string>().starts_with(id + ":"))
        deps.push_back("management/" + a["network"].get<std::string>());
    std::string operation = node.kind == "ovs-switch" ? "prepare-ovs-bridge"
                            : node.kind == "qemu"     ? "prepare-guest-resources"
                                                      : "prepare-gated-container";
    Json resource = {
        {"node", id}, {"kind", node.kind}, {"configuration", t.canonical["nodes"][id]}};
    if (node.kind == "ovs-switch")
      resource["failureDomain"] = "host/shared-ovs";
    else
      resource["artifact"] = t.artifacts["workloads"][node.workload];
    resource["managementAttachments"] = Json::array();
    for (const auto &a : t.canonical["managementAttachments"])
      if (a["endpoint"].get<std::string>().starts_with(id + ":"))
        resource["managementAttachments"].push_back(a);
    add("node/" + id, operation, deps, resource);
    preparation.push_back("node/" + id);
  }
  for (const auto &e : t.edges) {
    auto a = e.first.substr(0, e.first.find(':')), b = e.second.substr(0, e.second.find(':'));
    bool tap = t.nodes.at(a).kind == "qemu" || t.nodes.at(b).kind == "qemu";
    auto key = "edge/" + e.id;
    add(key, "prepare-disabled-link", {"node/" + a, "node/" + b},
        {{"edge", e.id},
         {"endpoints", Json::array({e.first, e.second})},
         {"primitive", tap ? "tap" : "veth"},
         {"adminUp", false}});
    preparation.push_back(key);
  }
  if (preparation.empty())
    preparation.push_back("preflight");
  add("policy", "configure-vlan-and-tree", preparation,
      {{"loopPolicy", t.canonical["loopPolicy"]}});
  std::vector<std::string> captures;
  // TAPs must exist before QEMU opens them. Disk/namespace preparation above
  // does not launch the VM; only this later step starts the paused process.
  for (const auto &[id, node] : t.nodes) {
    if (node.kind == "qemu") {
      const auto key = "guest/" + id + "/paused";
      add(key, "prepare-paused-guest", {"policy", "node/" + id}, {{"node", id}});
      captures.push_back(key);
    }
  }
  for (const auto &e : t.edges) {
    auto a = e.first.substr(0, e.first.find(':')), b = e.second.substr(0, e.second.find(':'));
    auto ak = t.nodes.at(a).kind, bk = t.nodes.at(b).kind;
    std::string endpoint = e.first, point = "endpoint0-host-veth";
    if (ak == "qemu" || bk == "qemu") {
      point = "guest-tap";
      endpoint = ak == "qemu" ? e.first : e.second;
    } else if (ak == "docker" && bk == "docker") {
      point = "endpoint0-container-veth";
    } else if (ak == "docker" || bk == "docker") {
      point = "workload-host-veth";
      endpoint = ak == "docker" ? e.first : e.second;
    }
    auto key = "capture/" + e.id;
    add(key, "arm-capture", {"policy", "edge/" + e.id},
        {{"edge", e.id},
         {"logicalEndpoint", endpoint},
         {"capturePoint", point},
         {"runtimeMappingRequired", true},
         {"directionMappingVerified", false},
         {"format", "pcapng"}});
    captures.push_back(key);
  }
  if (captures.empty())
    captures.push_back("policy");
  add("capture-barrier", "await-capture-activation", captures,
      {{"expectedCount", t.edges.size()}, {"required", t.canonical["capture"]["required"]}});
  add("enable-data", "enable-data-links", {"capture-barrier"});
  add("converge", "await-network-policy", {"enable-data"},
      {{"loopPolicy", t.canonical["loopPolicy"]}});
  std::vector<std::string> health;
  for (const auto &[id, node] : t.nodes)
    if (node.kind != "ovs-switch") {
      std::vector<std::string> deps{"converge", "node/" + id};
      if (node.kind == "qemu")
        deps.push_back("guest/" + id + "/paused");
      add("release/" + id, node.kind == "qemu" ? "continue-guest" : "release-workload", deps);
      add("health/" + id, "await-workload-readiness", {"release/" + id}, {{"node", id}});
      health.push_back("health/" + id);
    }
  if (health.empty())
    health.push_back("converge");
  add("ready", "mark-run-ready", health);
  // Kahn's algorithm over resource dependencies, never over cyclic network links.
  std::map<std::string, std::size_t> remaining;
  std::map<std::string, std::vector<std::string>> dependents;
  std::set<std::string> ready;
  for (const auto &[key, step] : steps) {
    remaining[key] = step["dependsOn"].size();
    if (remaining[key] == 0)
      ready.insert(key);
    for (const auto &dep : step["dependsOn"]) {
      auto name = dep.get<std::string>();
      if (!steps.contains(name))
        return std::unexpected(Error{"planner_error", key, "missing dependency: " + name});
      dependents[name].push_back(key);
    }
  }
  Json ordered = Json::array();
  while (!ready.empty()) {
    auto key = *ready.begin();
    ready.erase(ready.begin());
    ordered.push_back(steps.at(key));
    for (const auto &dependent : dependents[key])
      if (--remaining[dependent] == 0)
        ready.insert(dependent);
  }
  if (ordered.size() != steps.size())
    return std::unexpected(Error{"planner_error", "steps", "resource dependency cycle"});
  Json teardown = Json::array();
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
    const auto op = (*it)["operation"].get<std::string>();
    if (op.starts_with("prepare-") || op == "arm-capture")
      teardown.push_back((*it)["id"]);
  }
  return Json{
      {"apiVersion", "graphlab.plan/v1"},
      {"mode", "dry-run"},
      {"executable", false},
      {"topologyHash", t.hash},
      {"artifactLockHash", t.canonical["artifactLock"]},
      {"summary",
       {{"nodes", t.nodes.size()}, {"dataEdges", t.edges.size()}, {"captures", t.edges.size()}}},
      {"canonicalTopology", t.canonical},
      {"steps", ordered},
      {"resourceTeardownOrder", teardown},
      {"deferredChecks",
       Json::array({"artifact bytes/registry availability", "host routes and resource budgets",
                    "KVM/devices/privileges", "Docker/OVS/QEMU versions and ownership",
                    "capture direction and activation", "runtime generation and physical names"})}};
}
} // namespace graphlab
