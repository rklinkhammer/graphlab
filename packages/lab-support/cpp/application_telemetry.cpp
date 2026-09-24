#include <charconv>
#include <lab_support/application_telemetry.hpp>
#include <limits>
#include <regex>
namespace lab_support::application_telemetry {
namespace {
void require(bool v) {
  if (!v)
    throw std::invalid_argument("invalid_application_telemetry");
}
void fields(const Json &v, std::initializer_list<std::string_view> allowed) {
  require(v.is_object());
  for (const auto &[k, x] : v.items())
    require(std::find(allowed.begin(), allowed.end(), k) != allowed.end());
}
std::uint64_t number(const Json &v) {
  require(v.is_string());
  auto s = v.get<std::string>();
  require(!s.empty() && s.size() <= 20 && (s.size() == 1 || s.front() != '0'));
  std::uint64_t n{};
  auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
  require(e == std::errc{} && p == s.data() + s.size());
  return n;
}
constexpr auto counters = {"sentMessages",         "receivedMessages", "sentPayloadBytes",
                           "receivedPayloadBytes", "errors",           "rejectedMessages",
                           "backpressureEvents"};
} // namespace
Json validate(const Json &r) {
  require(r.dump().size() <= max_report_bytes);
  bool edge = r.at("apiVersion") == "graphlab.application-edge-telemetry/v1";
  if (edge) {
    fields(r, {"apiVersion", "stream", "epoch", "sequence", "elapsedNs", "counters", "latency",
               "edge", "endpoint"});
    require(r.at("edge").is_string() &&
            std::regex_match(r.at("edge").get<std::string>(),
                             std::regex("[A-Za-z][A-Za-z0-9_-]{0,63}")));
    require(r.at("endpoint") == "source" || r.at("endpoint") == "target");
  } else {
    fields(r, {"apiVersion", "stream", "epoch", "sequence", "elapsedNs", "counters", "latency"});
    require(r.at("apiVersion") == "graphlab.application-telemetry/v1");
  }
  require(r.at("stream").is_string() &&
          std::regex_match(r.at("stream").get<std::string>(), std::regex("[a-z][a-z0-9-]{0,31}")));
  require(r.at("epoch").is_string() &&
          std::regex_match(r.at("epoch").get<std::string>(), std::regex("[a-f0-9]{32}")));
  require(number(r.at("sequence")) > 0);
  number(r.at("elapsedNs"));
  const auto &c = r.at("counters");
  if (edge) {
    fields(c, {"sentMessages", "receivedMessages", "sentPayloadBytes", "receivedPayloadBytes",
               "errors", "rejectedMessages", "backpressureEvents", "reconnects", "backpressureNs"});
    for (auto key : {"reconnects", "backpressureNs"})
      if (!c.at(key).is_null())
        number(c.at(key));
    for (auto key : r.at("endpoint") == "source"
                        ? std::vector<std::string>{"receivedMessages", "receivedPayloadBytes"}
                        : std::vector<std::string>{"sentMessages", "sentPayloadBytes"})
      require(c.at(key).is_null());
  } else
    fields(c, {"sentMessages", "receivedMessages", "sentPayloadBytes", "receivedPayloadBytes",
               "errors", "rejectedMessages", "backpressureEvents"});
  for (auto key : counters)
    if (edge && c.at(key).is_null()) {
      require(std::string(key) == "backpressureEvents" ||
              (r.at("endpoint") == "target" &&
               (std::string(key) == "sentMessages" || std::string(key) == "sentPayloadBytes")) ||
              (r.at("endpoint") == "source" && (std::string(key) == "receivedMessages" ||
                                                std::string(key) == "receivedPayloadBytes")));
    } else
      number(c.at(key));
  if (r.contains("latency")) {
    const auto &l = r.at("latency");
    fields(l, {"kind", "count", "sumNs", "buckets"});
    require(l.at("kind") == "local-service-time");
    const auto count = number(l.at("count")), sum = number(l.at("sumNs"));
    const auto &b = l.at("buckets");
    require(b.is_array() && b.size() == 8);
    constexpr std::uint64_t upper[] = {10000, 50000, 100000, 500000, 1000000, 5000000, 10000000};
    std::uint64_t total = 0;
    long double minimum = 0, maximum = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      auto n = number(b[i]);
      require(n <= std::numeric_limits<std::uint64_t>::max() - total);
      total += n;
      minimum += static_cast<long double>(n) * (i ? upper[i - 1] + 1 : 0);
      if (i < 7)
        maximum += static_cast<long double>(n) * upper[i];
    }
    require(total == count &&
            count <= number(c.at(edge && r.at("endpoint") == "target" ? "receivedMessages"
                                                                      : "sentMessages")) &&
            minimum <= sum);
    require(number(b[7]) > 0 || sum <= maximum);
    require(count != 0 || sum == 0);
  }
  return r;
}
Json derive(const Json &old, const Json &input, const std::string &collector) {
  auto r = validate(input);
  Json result = {{"report", r},
                 {"collectorEpoch", collector},
                 {"rates", nullptr},
                 {"gapReason", nullptr},
                 {"latency", nullptr}};
  std::string gap;
  if (old.is_null())
    gap = "first_sample";
  else {
    const auto &p = old.at("report");
    if (p["epoch"] != r["epoch"])
      gap = "workload_restarted";
    else {
      require(number(r["sequence"]) > number(p["sequence"]));
      require(number(r["elapsedNs"]) > number(p["elapsedNs"]));
      require(p.at("apiVersion") == r.at("apiVersion") && p.at("stream") == r.at("stream"));
      if (r.contains("edge")) {
        require(p.at("edge") == r.at("edge") && p.at("endpoint") == r.at("endpoint"));
        for (auto key : {"reconnects", "backpressureNs"}) {
          require(p["counters"][key].is_null() == r["counters"][key].is_null());
          if (!r["counters"][key].is_null())
            require(number(r["counters"][key]) >= number(p["counters"][key]));
        }
      }
      for (auto key : counters) {
        require(r["counters"][key].is_null() == p["counters"][key].is_null());
        if (!r["counters"][key].is_null())
          require(number(r["counters"][key]) >= number(p["counters"][key]));
      }
      require(p.contains("latency") == r.contains("latency"));
      if (r.contains("latency")) {
        require(number(r["latency"]["sumNs"]) >= number(p["latency"]["sumNs"]));
        for (int i = 0; i < 8; ++i)
          require(number(r["latency"]["buckets"][i]) >= number(p["latency"]["buckets"][i]));
      }
      const auto delta = number(r["elapsedNs"]) - number(p["elapsedNs"]);
      if (old.value("collectorEpoch", "") != collector)
        gap = "collector_restarted";
      else if (delta > 15000000000ull)
        gap = "sample_gap";
      else {
        const double seconds = double(delta) / 1e9;
        Json rates = Json::object();
        for (auto key :
             {"sentMessages", "receivedMessages", "sentPayloadBytes", "receivedPayloadBytes"})
          if (r["counters"][key].is_null())
            rates[std::string(key) + "PerSecond"] = nullptr;
          else
            rates[std::string(key) + "PerSecond"] =
                double(number(r["counters"][key]) - number(p["counters"][key])) / seconds;
        result["rates"] = rates;
      }
    }
  }
  if (!gap.empty())
    result["gapReason"] = gap;
  if (r.contains("latency")) {
    const auto &l = r["latency"];
    const auto count = number(l["count"]);
    Json latency = {{"kind", "local-service-time"}, {"basis", "process-cumulative"},
                    {"count", l["count"]},          {"meanUs", nullptr},
                    {"p95UpperBoundUs", nullptr},   {"p95Overflow", false}};
    if (count) {
      latency["meanUs"] = double(number(l["sumNs"])) / double(count) / 1000;
      constexpr int upper[] = {10, 50, 100, 500, 1000, 5000, 10000};
      auto rank = count - count / 20;
      std::uint64_t cumulative = 0;
      for (int i = 0; i < 8; ++i) {
        cumulative += number(l["buckets"][i]);
        if (cumulative >= rank) {
          if (i == 7)
            latency["p95Overflow"] = true;
          else
            latency["p95UpperBoundUs"] = upper[i];
          break;
        }
      }
    }
    result["latency"] = latency;
  }
  return result;
}
} // namespace lab_support::application_telemetry
