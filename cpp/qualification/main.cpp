#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <lab_support/contracts.hpp>
#include <map>
#include <openssl/evp.h>
#include <set>
using lab_support::Json;
namespace fs = std::filesystem;
std::string hash(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("unreadable artifact: " + path.string());
  auto *ctx = EVP_MD_CTX_new();
  if (!ctx)
    throw std::runtime_error("digest allocation");
  struct Guard {
    EVP_MD_CTX *p;
    ~Guard() { EVP_MD_CTX_free(p); }
  } guard{ctx};
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("digest init");
  std::array<char, 65536> data{};
  while (in) {
    in.read(data.data(), data.size());
    if (EVP_DigestUpdate(ctx, data.data(), in.gcount()) != 1)
      throw std::runtime_error("digest update");
  }
  if (!in.eof())
    throw std::runtime_error("artifact read");
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned n;
  if (EVP_DigestFinal_ex(ctx, out, &n) != 1)
    throw std::runtime_error("digest final");
  std::string s = "sha256:";
  const char *digits = "0123456789abcdef";
  for (unsigned i = 0; i < n; ++i) {
    s += digits[out[i] >> 4];
    s += digits[out[i] & 15];
  }
  return s;
}
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "summarize") {
      auto input = lab_support::read_document(argv[2]);
      if (!input)
        throw std::runtime_error("unreadable capacity report");
      auto report = Json::parse(*input);
      if (report.at("apiVersion") != "graphlab.capacity/v1")
        throw std::runtime_error("unsupported capacity report");
      const bool sustained = report.value("mode", "") == "sustained-m6";
      struct Totals {
        double cpu = 0, rtt = 0, rate = 0, loss = 0, storage = 0, read = 0, write = 0;
        std::uint64_t rss = 0, drops = 0;
        unsigned count = 0;
        bool envelope = true;
        bool rtt_known = true, drops_known = true;
      };
      std::map<std::pair<int, std::string>, Totals> groups;
      std::map<std::string, std::uint64_t> prior_drops;
      std::set<std::string> unknown_drops;
      bool previous_capture_complete = true;
      for (const auto &s : report.at("samples")) {
        if (sustained && (s.at("durationSeconds") != 300 || !s.at("checks").is_object() ||
                          s.at("checks").size() != 8 || s.at("monitorCount") < 200))
          throw std::runtime_error("incomplete sustained observation");
        auto &a = groups[{s.at("targetRequestsPerSecond").get<int>(),
                          s.at("profile").get<std::string>()}];
        ++a.count;
        a.cpu += s.at("hostCpuBusyPercent").get<double>();
        if (s.at("rttP95Us").is_null())
          a.rtt_known = false;
        else
          a.rtt += s.at("rttP95Us").get<double>();
        a.rate += s.at("receivedRequestsPerSecond").get<double>();
        a.loss += s.at("lossFraction").get<double>();
        a.storage += s.at("captureBytesPerSecond").get<double>();
        a.read += s.at("hostPagingReadKiBPerSecond").get<double>();
        a.write += s.at("hostPagingWriteKiBPerSecond").get<double>();
        a.rss = std::max(
            a.rss, s.value("controllerRssMaxKiB", s.at("controllerRssKiB").get<std::uint64_t>()));
        a.envelope = a.envelope && s.at("withinSampleEnvelope").get<bool>();
        std::uint64_t interval_drops = 0;
        if (s.at("profile") == "capture" && !previous_capture_complete)
          a.drops_known = false;
        bool capture_complete = true;
        if (s.at("profile") == "capture" &&
            (!s.at("captureObservation").is_array() ||
             s.at("captureObservation").size() != report.at("edges").get<std::size_t>())) {
          a.drops_known = false;
          capture_complete = false;
          for (const auto &[id, count] : prior_drops)
            unknown_drops.insert(id);
        }
        std::set<std::string> observed_workers;
        for (const auto &capture : s.at("captureObservation"))
          if (capture.contains("statistics") && capture["statistics"].is_object() &&
              capture["statistics"].contains("dropped") &&
              !capture["statistics"]["dropped"].is_null()) {
            const auto id = capture.at("id").get<std::string>();
            if (!observed_workers.insert(id).second)
              throw std::runtime_error("duplicate capture observation");
            const auto drops = capture["statistics"]["dropped"].get<std::uint64_t>();
            if (unknown_drops.erase(id))
              a.drops_known = false;
            if (drops < prior_drops[id])
              throw std::runtime_error("capture counter reset in capacity report");
            interval_drops += drops - prior_drops[id];
            prior_drops[id] = drops;
          } else {
            a.drops_known = false;
            capture_complete = false;
            unknown_drops.insert(capture.at("id").get<std::string>());
          }
        if (s.at("profile") == "capture")
          previous_capture_complete = capture_complete;
        a.drops = std::max(a.drops, interval_drops);
      }
      for (int rate : sustained ? std::vector<int>{1000} : std::vector<int>{250, 1000, 4000})
        for (const auto profile : {"baseline", "telemetry", "capture"})
          if (!groups.contains({rate, profile}) || groups.at({rate, profile}).count != 2)
            throw std::runtime_error("incomplete capacity matrix");
      if (groups.size() != (sustained ? 3 : 9))
        throw std::runtime_error("unexpected capacity matrix");
      std::cout
          << "# Capacity measurements\n\n"
          << report.at("shape").get<std::string>()
          << ". Two samples per profile/rate; means below (RTT is the mean of sample p95s).\n\n"
          << "| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU "
             "Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | "
             "Max worker drop increment | Delivery envelope | Capture observations |\n"
          << "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|\n"
          << std::fixed << std::setprecision(2);
      for (const auto &[key, a] : groups) {
        auto it = groups.find({key.first, "baseline"});
        if (it == groups.end())
          throw std::runtime_error("missing baseline");
        const auto &b = it->second;
        auto cpu = a.cpu / a.count, rtt = a.rtt / a.count, base = b.rtt / b.count;
        std::cout << "| " << key.first << " | " << key.second << " | " << a.rate / a.count << " | "
                  << 100 * a.loss / a.count << " | ";
        if (a.rtt_known)
          std::cout << rtt;
        else
          std::cout << "unknown";
        std::cout << " | " << cpu << " | " << cpu - b.cpu / b.count << " | ";
        if (a.rtt_known && b.rtt_known && base > 0)
          std::cout << 100 * (rtt / base - 1);
        else
          std::cout << "unknown";
        std::cout << " | " << a.rss << " | " << a.read / a.count << " / " << a.write / a.count
                  << " | " << a.storage / a.count / 1024 << " | ";
        if (a.drops_known)
          std::cout << a.drops;
        else
          std::cout << "unknown";
        std::cout << " | " << (a.envelope ? "pass" : "outside") << " | "
                  << (key.second != "capture" ? "not enabled"
                      : !a.drops_known        ? "incomplete"
                      : a.drops               ? "drops observed"
                                              : "no drops observed")
                  << " |\n";
      }
      std::cout << "\nCPU and I/O cover the whole VM. Worker drops are increments since the "
                   "previous observation (first since activation), separate "
                   "from delivery loss. A passing envelope applies only to the sampled duration "
                   "and topology; no long-duration saturation bound is inferred.\n";
      if (sustained)
        std::cout << "\nSustained mode: two 300-second samples per profile at 1,000 requests/s. "
                     "The envelope column includes the declared CPU, memory, RTT, continuity and "
                     "source-drop checks. "
                     "Unavailable interface-drop counters remain unknown. Raw reports retain "
                     "one-second health samples.\n";
      return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "record") {
      const auto root = fs::canonical(argv[2]);
      auto text = lab_support::read_document(argv[3]);
      if (!text)
        throw std::runtime_error("invalid matrix");
      auto parsed = lab_support::parse_document(*text);
      if (!parsed)
        throw std::runtime_error("invalid matrix");
      auto manifest = *parsed;
      manifest["artifacts"] = Json::array();
      for (const auto &p : fs::directory_iterator(root))
        if (p.is_regular_file() && !p.is_symlink() && p.path().filename() != "manifest.json" &&
            p.path().filename() != "result.json")
          manifest["artifacts"].push_back(
              {{"path", p.path().filename().string()}, {"sha256", hash(p.path())}});
      const auto output = root / "manifest.json";
      if (fs::exists(output))
        throw std::runtime_error("manifest already exists");
      std::ofstream out(output);
      out << manifest.dump(2) << '\n';
      if (!out)
        throw std::runtime_error("manifest write failed");
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "hash") {
      std::cout << hash(argv[2]) << '\n';
      return 0;
    }
    if (argc != 3 || std::string(argv[1]) != "verify") {
      std::cerr << "Usage: lab-qualify hash FILE | record DIRECTORY MATRIX.json | verify "
                   "MANIFEST.json | summarize CAPACITY.json\n";
      return 2;
    }
    auto parsed = lab_support::read_document(argv[2]);
    if (!parsed)
      throw std::runtime_error("invalid manifest document");
    auto document = lab_support::parse_document(*parsed);
    if (!document)
      throw std::runtime_error("invalid manifest structure");
    const auto &m = *document;
    if (m.at("apiVersion") != "graphlab.qualification/v1")
      throw std::runtime_error("unsupported qualification version");
    if (!m.at("artifacts").is_array() || !m.at("gates").is_array())
      throw std::runtime_error("artifact and gate arrays required");
    const auto root = fs::canonical(fs::absolute(argv[2]).parent_path());
    std::set<std::string> evidence;
    for (const auto &a : m.at("artifacts")) {
      const auto name = a.at("path").get<std::string>();
      fs::path relative(name);
      if (relative.empty() || relative.is_absolute())
        throw std::runtime_error("invalid artifact path");
      const auto resolved = fs::canonical(root / relative);
      auto suffix = resolved.lexically_relative(root);
      if (suffix.empty() || *suffix.begin() == ".." || !fs::is_regular_file(resolved))
        throw std::runtime_error("artifact outside evidence directory");
      if (!evidence.insert(name).second)
        throw std::runtime_error("duplicate artifact");
      if (hash(resolved) != a.at("sha256").get<std::string>())
        throw std::runtime_error("artifact digest mismatch: " + name);
    }
    for (const auto key : {"runtimeLock", "sourceArchive", "workloadArchive", "capacityReport"})
      if (!evidence.contains(m.at(key).get<std::string>()))
        throw std::runtime_error(std::string("unbound ") + key);
    std::set<std::string> gates;
    Json incomplete = Json::array();
    for (const auto &g : m.at("gates")) {
      if (!g.at("evidence").is_array())
        throw std::runtime_error("gate evidence array required");
      auto id = g.at("id").get<std::string>();
      if (id.size() != 3 || id[0] != 'T' || id[1] < '0' || id[1] > '1' || id[2] < '0' ||
          id[2] > '9' || id == "T00" || !gates.insert(id).second)
        throw std::runtime_error("invalid/duplicate gate");
      auto status = g.at("status").get<std::string>();
      if (status != "passed" && status != "partial" && status != "blocked" && status != "failed")
        throw std::runtime_error("invalid gate status");
      if (g.at("reason").get<std::string>().empty())
        throw std::runtime_error("gate needs explanation");
      if (status == "passed" && g.at("evidence").empty())
        throw std::runtime_error("passed gate has no evidence");
      for (const auto &p : g.at("evidence"))
        if (!evidence.contains(p.get<std::string>()))
          throw std::runtime_error("unbound gate evidence");
      if (status != "passed")
        incomplete.push_back(id);
    }
    if (gates.size() != 19)
      throw std::runtime_error("all T01-T19 gates are required");
    const bool qualified = incomplete.empty();
    std::cout << Json{{"apiVersion", "graphlab.qualification-result/v1"},
                      {"integrityVerified", true},
                      {"qualified", qualified},
                      {"incompleteGates", incomplete},
                      {"manifestSha256", hash(argv[2])}}
                     .dump(2)
              << '\n';
    return qualified ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << Json{{"integrityVerified", false}, {"error", e.what()}}.dump() << '\n';
    return 2;
  }
}
