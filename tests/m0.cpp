#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <graphlab/planner.hpp>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
using namespace lab_support;
namespace {
int passed = 0;
void check(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
void test(const std::string &name, const std::function<void()> &body) {
  body();
  ++passed;
  std::cout << "PASS " << name << '\n';
}
std::string fakehash(char c) { return "sha256:" + std::string(64, c); }
Json contract(std::string name, std::string kind) {
  Json interfaces = Json::object();
  for (auto id : {"data0", "data1", "mgmt0"})
    interfaces[id] = {{"role", std::string(id) == "mgmt0" ? "management" : "data"},
                      {"required", std::string(id) != "data1"},
                      {"medium", "ethernet"},
                      {"mtuRange", Json::array({1280, 9000})}};
  return {{"apiVersion", "graphlab.workload/v2"},
          {"id", name},
          {"kind", kind},
          {"platforms", Json::array({"linux/arm64", "linux/amd64"})},
          {"interfaces", interfaces},
          {"lifecycle", {{"gateUntilRelease", true}, {"quiesce", "supported"}}},
          {"labSupport",
           {{"language", "cpp23"}, {"version", "1.0.0"}, {"packageSha256", fakehash('a')}}},
          {"resources", {{"cpus", 1}, {"memoryMiB", 256}}}};
}
Json artifacts() {
  Json entries = Json::object();
  for (auto name : {"app-a", "app-b", "guest-linux"}) {
    bool guest = std::string(name) == "guest-linux";
    auto c = contract(name, guest ? "qemu" : "docker");
    Json e = {{"contract", c}, {"contractSha256", digest(c)}, {"platform", "linux/arm64"}};
    if (guest) {
      e["diskSha256"] = fakehash('b');
      e["vm"] = {
          {"machine", "virt-8.2"}, {"firmwareSha256", fakehash('c')}, {"accelerator", "kvm"}};
    } else
      e["image"] = "registry.invalid/" + std::string(name) + "@" + fakehash('d');
    entries[name] = e;
  }
  return {{"apiVersion", "graphlab.artifacts/v1"}, {"workloads", entries}};
}
Json switch_node() {
  return {{"kind", "ovs-switch"},
          {"policy", {{"forwarding", "normal"}, {"rstp", true}, {"priority", 4096}}},
          {"ports", Json::object()}};
}
void edge(Json &t, std::string key, std::string a, std::string ap, std::string b, std::string bp) {
  for (const auto &pair : {std::pair{a, ap}, std::pair{b, bp}}) {
    auto &node = t["nodes"][pair.first];
    if (node["kind"] == "ovs-switch")
      node["ports"][pair.second] = {{"role", "data"},
                                    {"medium", "ethernet"},
                                    {"mtu", 1500},
                                    {"vlan", {{"trunk", Json::array({100, 200})}}}};
  }
  t["edges"].push_back({{"id", key}, {"endpoints", Json::array({a + ":" + ap, b + ":" + bp})}});
}
Json base(const Json &lock) {
  return {{"apiVersion", "graphlab.topology/v2"},
          {"id", "fixture"},
          {"artifactLock", digest(lock)},
          {"backend", {{"kind", "linux-local"}, {"switchIsolation", "shared-ovs"}}},
          {"management", {{"networks", Json::object()}}},
          {"managementAttachments", Json::array()},
          {"nodes", Json::object()},
          {"edges", Json::array()},
          {"capture", {{"required", true}, {"scope", "all-data-edges"}, {"format", "pcapng"}}}};
}
Json shape(const std::string &name, const Json &lock) {
  Json t = base(lock);
  t["id"] = name;
  int count = (name == "mesh" || name == "star") ? 4 : 3;
  for (int i = 1; i <= count; ++i)
    t["nodes"]["s" + std::to_string(i)] = switch_node();
  auto connect = [&](int a, int b, std::string suffix = "") {
    auto sa = std::to_string(a), sb = std::to_string(b);
    edge(t, "l" + sa + sb + suffix, "s" + sa, "p" + sb + suffix, "s" + sb, "p" + sa + suffix);
  };
  if (name == "isolated")
    return t;
  connect(1, 2);
  if (name == "disconnected")
    return t;
  if (name == "star") {
    connect(1, 3);
    connect(1, 4);
  } else if (name == "mesh") {
    connect(1, 3);
    connect(1, 4);
    connect(2, 3);
    connect(2, 4);
    connect(3, 4);
  } else {
    connect(2, 3);
    if (name != "chain")
      connect(3, 1);
    if (name == "parallel")
      connect(1, 2, "b");
  }
  return t;
}
Json triangle(const Json &lock) {
  auto t = shape("ring", lock);
  t["id"] = "triangle";
  t["management"]["networks"]["mgmt"] = {{"subnet", "172.30.80.0/24"},
                                         {"dynamicPool", "172.30.80.128/25"},
                                         {"gateway", "172.30.80.1"},
                                         {"externalAccess", false}};
  int index = 0;
  for (auto name : {"a", "b", "g"}) {
    ++index;
    std::string workload = std::string(name) == "a"   ? "app-a"
                           : std::string(name) == "b" ? "app-b"
                                                      : "guest-linux";
    t["nodes"][name] = {{"kind", index == 3 ? "qemu" : "docker"},
                        {"workload", workload},
                        {"ports",
                         {{"data0", {{"role", "data"}, {"medium", "ethernet"}, {"mtu", 1500}}},
                          {"mgmt0", {{"role", "management"}}}}}};
    t["managementAttachments"].push_back(
        {{"endpoint", std::string(name) + ":mgmt0"},
         {"network", "mgmt"},
         {"address", "172.30.80." + std::to_string(10 + index) + "/24"}});
    edge(t, std::string(name) + "-s", name, "data0", "s" + std::to_string(index), "workload");
    t["nodes"]["s" + std::to_string(index)]["ports"]["workload"]["vlan"] = {{"access", 100}};
  }
  return t;
}
void dag(const Json &p) {
  std::set<std::string> visited;
  for (const auto &step : p["steps"]) {
    for (const auto &dep : step["dependsOn"])
      check(visited.contains(dep.get<std::string>()), "dependency not ordered");
    check(visited.insert(step["id"].get<std::string>()).second, "duplicate step");
    if (step["operation"] == "enable-data-links")
      check(visited.contains("capture-barrier"), "links before capture barrier");
    if (step["operation"] == "continue-guest" || step["operation"] == "release-workload")
      check(visited.contains("converge"), "release before convergence");
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    auto lock = artifacts();
    auto t = triangle(lock);
    if (argc == 3 && std::string(argv[1]) == "--write-fixtures") {
      std::filesystem::create_directories(argv[2]);
      auto save = [&](std::string path, const Json &j) {
        std::ofstream(std::filesystem::path(argv[2]) / path) << j.dump(2) << '\n';
      };
      save("artifacts.lock.json", lock);
      save("triangle.yaml", t);
      for (auto name : {"chain", "star", "ring", "mesh", "disconnected", "isolated", "parallel"})
        save(std::string(name) + ".yaml", shape(name, lock));
      save("app-a.workload.json", lock["workloads"]["app-a"]["contract"]);
      return 0;
    }
    test("SHA-256 known canonical empty object", [] {
      check(digest(Json::object()) ==
                "sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a",
            "hash mismatch");
    });
    test("YAML scalar typing and quoted values", [] {
      auto r = parse_document("name: 'true'\nflag: true\nnumber: 12\n");
      check(r && (*r)["name"].is_string() && (*r)["flag"].is_boolean() && (*r)["number"] == 12,
            "wrong scalar resolution");
    });
    for (auto name : {"chain", "star", "ring", "mesh", "disconnected", "isolated", "parallel"})
      test(name, [&] {
        auto s = shape(name, lock);
        auto p = graphlab::plan(s, lock);
        check(p.has_value(), p ? "" : p.error().message);
        dag(*p);
        check((*p)["summary"]["captures"] == s["edges"].size(), "capture count hardcoded");
      });
    test("mixed Docker QEMU triangle", [&] {
      auto p = graphlab::plan(t, lock);
      check(p.has_value(), p ? "" : p.error().message);
      dag(*p);
      check((*p)["summary"]["captures"] == 6, "triangle capture count");
      std::set<std::string> previous;
      for (const auto &step : (*p)["steps"]) {
        if (step["operation"] == "prepare-paused-guest")
          check(previous.contains("edge/g-s"), "QEMU opens TAP before preparation");
        previous.insert(step["id"].get<std::string>());
      }
    });
    test("generated variable-size graphs and deterministic plans", [&] {
      std::mt19937 random(42);
      for (int count = 1; count <= 20; ++count) {
        auto graph = base(lock);
        for (int i = 0; i < count; ++i)
          graph["nodes"]["s" + std::to_string(i)] = switch_node();
        if (count > 1)
          for (int i = 0; i < count * 2; ++i) {
            int a = static_cast<int>(random() % count),
                b = (a + 1 + static_cast<int>(random() % (count - 1))) % count;
            auto suffix = std::to_string(i);
            edge(graph, "e" + suffix, "s" + std::to_string(a), "p" + suffix,
                 "s" + std::to_string(b), "p" + suffix);
          }
        auto first = graphlab::plan(graph, lock);
        check(first.has_value(), "generated graph rejected");
        dag(*first);
        std::shuffle(graph["edges"].begin(), graph["edges"].end(), random);
        auto second = graphlab::plan(graph, lock);
        check(second && *first == *second, "generated graph plan unstable");
      }
    });
    test("multi-interface workload", [&] {
      auto s = t;
      s["nodes"]["a"]["ports"]["data1"] = {{"role", "data"}};
      edge(s, "extra", "a", "data1", "s2", "extra");
      check(validate(s, lock).has_value(), "multi-interface rejected");
    });
    test("direct Docker cable", [&] {
      auto s = t;
      s["nodes"]["a"]["ports"]["data1"] = {{"role", "data"}};
      s["nodes"]["b"]["ports"]["data1"] = {{"role", "data"}};
      edge(s, "direct", "a", "data1", "b", "data1");
      check(graphlab::plan(s, lock).has_value(), "direct Docker rejected");
    });
    test("deterministic YAML key/edge/attachment/trunk ordering", [&] {
      auto s = t;
      std::reverse(s["edges"].begin(), s["edges"].end());
      std::reverse(s["managementAttachments"].begin(), s["managementAttachments"].end());
      for (auto &[id, n] : s["nodes"].items())
        for (auto &[pid, p] : n["ports"].items())
          if (p.contains("vlan") && p["vlan"].contains("trunk"))
            std::reverse(p["vlan"]["trunk"].begin(), p["vlan"]["trunk"].end());
      check(*graphlab::plan(t, lock) == *graphlab::plan(s, lock), "unstable plan");
      auto parsed = parse_document(t.dump());
      check(parsed && *graphlab::plan(*parsed, lock) == *graphlab::plan(t, lock),
            "roundtrip unstable");
    });
    test("default normalization", [&] {
      auto s = t;
      s["nodes"]["a"]["ports"]["data0"].erase("mtu");
      s["nodes"]["a"]["ports"]["data0"].erase("medium");
      check(validate(s, lock)->hash == validate(t, lock)->hash, "default hash changed");
    });
    auto reject = [&](const std::string &name, const std::function<void(Json &)> &change) {
      test(name, [&] {
        auto s = t;
        change(s);
        check(!validate(s, lock), "invalid topology accepted");
      });
    };
    reject("duplicate edge ID", [](Json &s) { s["edges"].push_back(s["edges"][0]); });
    reject("missing endpoint", [](Json &s) { s["edges"][0]["endpoints"][0] = "missing:p"; });
    reject("reused physical port",
           [](Json &s) { s["edges"][1]["endpoints"][0] = s["edges"][0]["endpoints"][0]; });
    reject("endpoint arity", [](Json &s) { s["edges"][0]["endpoints"].push_back("s1:p2"); });
    reject("VLAN bounds",
           [](Json &s) { s["nodes"]["s1"]["ports"]["workload"]["vlan"]["access"] = 4095; });
    reject("trunk duplicates", [](Json &s) {
      s["nodes"]["s1"]["ports"]["p2"]["vlan"]["trunk"] = Json::array({100, 100});
    });
    reject("MTU mismatch", [](Json &s) { s["nodes"]["s1"]["ports"]["workload"]["mtu"] = 1400; });
    reject("missing required workload interface",
           [](Json &s) { s["nodes"]["a"]["ports"].erase("data0"); });
    reject("undeclared workload interface",
           [](Json &s) { s["nodes"]["a"]["ports"]["typo"] = {{"role", "data"}}; });
    reject("wrong workload kind", [](Json &s) { s["nodes"]["a"]["kind"] = "qemu"; });
    reject("unresolved workload", [](Json &s) { s["nodes"]["a"]["workload"] = "absent"; });
    reject("unresolved lock digest", [](Json &s) { s["artifactLock"] = "sha256:<digest>"; });
    reject("wrong lock digest", [](Json &s) { s["artifactLock"] = fakehash('0'); });
    reject("unknown major", [](Json &s) { s["apiVersion"] = "graphlab.topology/v999"; });
    reject("unknown required feature",
           [](Json &s) { s["requiredFeatures"] = Json::array({"wireless"}); });
    reject("unknown field", [](Json &s) { s["caputre"] = s["capture"]; });
    reject("node budget", [](Json &s) { s["limits"] = {{"maxNodes", 1}, {"maxEdges", 100}}; });
    reject("edge budget", [](Json &s) { s["limits"] = {{"maxNodes", 100}, {"maxEdges", 0}}; });
    reject("unsupported isolation",
           [](Json &s) { s["backend"]["switchIsolation"] = "per-switch"; });
    reject("management/data confusion", [](Json &s) { s["edges"][3]["endpoints"][0] = "a:mgmt0"; });
    reject("management pool conflict",
           [](Json &s) { s["managementAttachments"][0]["address"] = "172.30.80.129/24"; });
    reject("management duplicate IP", [](Json &s) {
      s["managementAttachments"][1]["address"] = s["managementAttachments"][0]["address"];
    });
    reject("management unknown network",
           [](Json &s) { s["managementAttachments"][0]["network"] = "absent"; });
    reject("management subnet overlap", [](Json &s) {
      s["management"]["networks"]["other"] = s["management"]["networks"]["mgmt"];
    });
    reject("cyclic RSTP policy", [](Json &s) { s["nodes"]["s1"]["policy"]["rstp"] = false; });
    test("explicit unprotected loops", [&] {
      auto s = t;
      s["loopPolicy"] = "unprotected";
      s["nodes"]["s1"]["policy"]["rstp"] = false;
      check(validate(s, lock).has_value(), "explicit loop rejected");
    });
    test("guest to guest capability rejection", [&] {
      auto s = base(lock);
      for (auto id : {"g", "h"})
        s["nodes"][id] = t["nodes"]["g"];
      edge(s, "guest", "g", "data0", "h", "data0");
      auto result = validate(s, lock);
      check(!result && result.error().code == "unsupported_backend", "wrong capability result");
    });
    test("tampered contract", [&] {
      auto l = lock;
      l["workloads"]["app-a"]["contract"]["resources"]["cpus"] = 2;
      auto s = t;
      s["artifactLock"] = digest(l);
      auto r = validate(s, l);
      check(!r && r.error().code == "integrity_error", "tampered contract accepted");
    });
    test("mutable image rejected", [&] {
      auto l = lock;
      l["workloads"]["app-a"]["image"] = "registry.invalid/app:latest";
      auto s = t;
      s["artifactLock"] = digest(l);
      check(!validate(s, l), "mutable image accepted");
    });
    test("contract language and major", [&] {
      auto c = contract("a", "docker");
      c["labSupport"]["language"] = "python";
      check(!validate_workload(c), "Python support accepted");
      c = contract("a", "docker");
      c["apiVersion"] = "graphlab.workload/v3";
      check(!validate_workload(c), "unknown contract major accepted");
    });
    for (const auto &text : std::vector<std::string>{
             "x: 1\nx: 2\n", "{\"x\":1,\"x\":2}", "x: &a [1]\ny: *a\n", "x: !custom value\n",
             "x: !!str value\n", "x: 1\n---\ny: 2\n", "? [a,b]\n: value\n", "x: *missing\n"})
      test("unsafe or ambiguous document rejected",
           [&] { check(!parse_document(text), "invalid document accepted: " + text); });
    test("depth limit", [] {
      std::string x = "x: ";
      for (int i = 0; i < 70; ++i)
        x += '[';
      x += '0';
      for (int i = 0; i < 70; ++i)
        x += ']';
      check(!parse_document(x), "deep document accepted");
    });
    test("byte limit", [] {
      check(!parse_document(std::string(1024 * 1024 + 1, ' ')), "oversized document accepted");
    });
    test("planning does not mutate inputs", [&] {
      auto before = t.dump(), lb = lock.dump();
      auto p = graphlab::plan(t, lock);
      check(p && t.dump() == before && lock.dump() == lb, "input modified");
      check((*p)["executable"] == false, "plan claimed executable");
    });
    std::cout << passed << " acceptance cases passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
