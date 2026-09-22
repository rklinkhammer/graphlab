#include <algorithm>
#include <graphlab/console.hpp>
#include <iomanip>
#include <openssl/rand.h>
#include <sstream>
#include <sys/utsname.h>

namespace graphlab::console {
std::string random_hex(std::size_t bytes) {
  std::vector<unsigned char> data(bytes);
  if (RAND_bytes(data.data(), static_cast<int>(bytes)) != 1)
    throw std::runtime_error("random_failed");
  std::ostringstream out;
  for (auto byte : data)
    out << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
  return out.str();
}
Json load(const std::filesystem::path &path) {
  auto data = lab_support::read_document(path);
  if (!data)
    throw std::runtime_error(data.error().code + ": " + path.string());
  auto parsed = lab_support::parse_document(*data);
  if (!parsed)
    throw std::runtime_error(parsed.error().code + ": " + path.string());
  return *parsed;
}
std::string timestamp() {
  auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
  gmtime_r(&now, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%FT%TZ");
  return out.str();
}
Catalog::Catalog(const std::filesystem::path &directory, const std::filesystem::path &lock)
    : loaded_(timestamp()) {
  auto artifacts = load(lock);
  artifacts_ = artifacts;
  std::size_t total = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    // Ignore editor/OS metadata (including macOS AppleDouble sidecars).
    if (entry.path().filename().string().starts_with('.') || !entry.is_regular_file())
      continue;
    auto extension = entry.path().extension();
    if (extension != ".yaml" && extension != ".yml")
      continue;
    if (revisions_.size() >= 64)
      throw std::runtime_error("catalog exceeds 64 revisions");
    auto validated = lab_support::validate(load(entry.path()), artifacts);
    if (!validated)
      throw std::runtime_error(entry.path().filename().string() + ": " + validated.error().path +
                               ": " + validated.error().message);
    total += validated->canonical.dump().size();
    if (total > 4 * 1024 * 1024)
      throw std::runtime_error("catalog exceeds 4 MiB");
    revisions_[validated->hash] = validated->canonical;
  }
  if (revisions_.empty())
    throw std::runtime_error("no YAML topology revisions found");
}
Json Catalog::resolve(const std::string &hash) const {
  auto found = revisions_.find(hash);
  if (found == revisions_.end())
    throw std::runtime_error("unknown_topology");
  return {{"topology", found->second}, {"artifacts", artifacts_}};
}
Json Catalog::dispatch(const Json &r) const {
  if (!r.is_object() || r.size() != 3 || r.value("apiVersion", "") != "graphlab.rpc/v1" ||
      !r.contains("method") || !r["method"].is_string() || !r.contains("params") ||
      !r["params"].is_object())
    throw std::runtime_error("invalid_request");
  const auto method = r["method"].get<std::string>();
  const auto &params = r["params"];
  if (method != "inventory" && !params.empty())
    throw std::runtime_error("invalid_params");
  if (method == "capabilities")
    return {{"apiVersion", "graphlab.capabilities/v1"},
            {"readOnly", true},
            {"execution", false},
            {"runtimeMappings", false},
            {"captures", false},
            {"terminals", false},
            {"backend", "linux-local/shared-ovs"},
            {"topologySchema", "graphlab.topology/v2"}};
  if (method == "topologies") {
    Json items = Json::array();
    for (const auto &[hash, t] : revisions_)
      items.push_back({{"hash", hash},
                       {"id", t["id"]},
                       {"nodes", t["nodes"].size()},
                       {"edges", t["edges"].size()}});
    return {{"apiVersion", "graphlab.catalog/v1"}, {"loadedAt", loaded_}, {"items", items}};
  }
  if (method == "diagnostics") {
    utsname host{};
    if (uname(&host))
      throw std::runtime_error("observation_failed");
    return {
        {"apiVersion", "graphlab.diagnostics/v1"},
        {"observedAt", timestamp()},
        {"host", {{"os", host.sysname}, {"architecture", host.machine}, {"kernel", host.release}}},
        {"catalogLoadedAt", loaded_},
        {"revisionCount", revisions_.size()},
        {"runtimeQualified", false},
        {"checks",
         Json::array(
             {{{"id", "contracts"}, {"state", "valid"}},
              {{"id", "runtime-mappings"},
               {"state", "unknown"},
               {"reason", "No executor-owned resource identities in M1"}},
              {{"id", "ovs"}, {"state", "unknown"}, {"reason", "Shared daemon not queried"}},
              {{"id", "artifact-bytes"},
               {"state", "unknown"},
               {"reason", "Only local contract and lock integrity checked"}}})}};
  }
  if (method != "inventory")
    throw std::runtime_error("unsupported_method");
  if (params.size() != 1 || !params.contains("hash") || !params["hash"].is_string())
    throw std::runtime_error("invalid_params");
  auto it = revisions_.find(params["hash"].get<std::string>());
  if (it == revisions_.end())
    throw std::runtime_error("not_found");
  const auto &[hash, topology] = *it;
  auto unknown = [] {
    return Json{{"state", "unknown"},
                {"observedAt", nullptr},
                {"mappingEpoch", nullptr},
                {"identity", nullptr},
                {"reason", "No executor-owned runtime mapping"}};
  };
  Json nodes = Json::array(), edges = Json::array();
  for (const auto &[id, n] : topology["nodes"].items())
    nodes.push_back(
        {{"id", id},
         {"kind", n["kind"]},
         {"configuration", n},
         {"runtime", unknown()},
         {"failureDomain", n["kind"] == "ovs-switch" ? Json("host/shared-ovs") : Json(nullptr)}});
  for (const auto &e : topology["edges"])
    edges.push_back({{"id", e["id"]},
                     {"endpoints", e["endpoints"]},
                     {"runtime", unknown()},
                     {"adminState", "unknown"},
                     {"carrierState", "unknown"},
                     {"rstpState", "unknown"},
                     {"captureState", "unknown"},
                     {"rates", {{"endpoint0To1", nullptr}, {"endpoint1To0", nullptr}}}});
  return {{"apiVersion", "graphlab.inventory/v1"},
          {"topologyHash", hash},
          {"topologyId", topology["id"]},
          {"generatedAt", timestamp()},
          {"catalogLoadedAt", loaded_},
          {"runtimeObservedAt", nullptr},
          {"runtimeFreshness", "unknown"},
          {"run", nullptr},
          {"capturePolicy", topology["capture"]},
          {"nodes", nodes},
          {"edges", edges},
          {"management", topology["management"]},
          {"managementAttachments", topology["managementAttachments"]},
          {"failureDomains",
           Json::array({{{"id", "host/shared-ovs"},
                         {"state", "unknown"},
                         {"description", "All logical OVS switches share one host daemon; a bridge "
                                         "outage and a daemon outage have different scope."}}})}};
}
} // namespace graphlab::console
