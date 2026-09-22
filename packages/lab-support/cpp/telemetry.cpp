#include <charconv>
#include <lab_support/telemetry.hpp>
namespace lab_support::telemetry {
namespace {
std::uint64_t number(const Json &v) {
  auto s = v.get<std::string>();
  std::uint64_t n{};
  auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (e != std::errc{} || p != s.data() + s.size() || s.empty())
    throw std::invalid_argument("invalid_counter");
  return n;
}
} // namespace
Json derive(const Json &old, Json s) {
  s["forwardBitsPerSecond"] = nullptr;
  s["reverseBitsPerSecond"] = nullptr;
  s["forwardPacketsPerSecond"] = nullptr;
  s["reversePacketsPerSecond"] = nullptr;
  std::string gap;
  if (!s.value("valid", false))
    gap = s.value("reason", "source_unavailable");
  else if (old.is_null() || !old.value("valid", false))
    gap = "first_sample";
  else if (s["mappingEpoch"] != old["mappingEpoch"] || s["bootId"] != old["bootId"] ||
           s["collectorEpoch"] != old["collectorEpoch"])
    gap = "identity_changed";
  else {
    auto now = number(s["monotonicNs"]), before = number(old["monotonicNs"]);
    if (now <= before || now - before > 3000000000ull)
      gap = "sample_gap";
    for (auto key : {"rxBytes", "txBytes", "rxPackets", "txPackets", "rxErrors", "txErrors",
                     "rxDropped", "txDropped"})
      if (number(s["raw"][key]) < number(old["raw"][key]))
        gap = "counter_reset";
    if (gap.empty()) {
      double seconds = double(now - before) / 1e9;
      bool rx = s["forwardMetric"] == "rx";
      auto rate = [&](const char *key, double scale) {
        return scale * double(number(s["raw"][key]) - number(old["raw"][key])) / seconds;
      };
      s["forwardBitsPerSecond"] = rate(rx ? "rxBytes" : "txBytes", 8);
      s["reverseBitsPerSecond"] = rate(rx ? "txBytes" : "rxBytes", 8);
      s["forwardPacketsPerSecond"] = rate(rx ? "rxPackets" : "txPackets", 1);
      s["reversePacketsPerSecond"] = rate(rx ? "txPackets" : "rxPackets", 1);
    }
  }
  auto epoch = old.is_null() ? 0 : number(old.at("counterEpoch"));
  s["counterEpoch"] = std::to_string(epoch + !gap.empty());
  s["gapReason"] = gap.empty() ? Json(nullptr) : Json(gap);
  return s;
}
} // namespace lab_support::telemetry
