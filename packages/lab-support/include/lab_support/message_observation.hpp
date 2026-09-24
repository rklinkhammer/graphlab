#pragma once
#include <array>
#include <chrono>
#include <deque>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
namespace lab_support::messages {
using Json = nlohmann::json;
// GLM1 + origin epoch (32 lowercase hex) + message sequence (16 hex) + stream A/B.
// Echo responses retain the same identifier: multiple packet occurrences are ambiguous.
inline Json identifier(std::string_view wire) {
  if (wire.size() != 53 || wire.substr(0, 4) != "GLM1" ||
      (wire.back() != 'A' && wire.back() != 'B' && wire.back() != 'a' && wire.back() != 'b'))
    return nullptr;
  std::string id(wire.substr(4, 48));
  if (!std::regex_match(id, std::regex("[a-f0-9]{48}")))
    return nullptr;
  return {{"messageId", id},
          {"traceId", id},
          {"stream", (wire.back() == 'A' || wire.back() == 'a') ? "alpha" : "beta"},
          {"phase", wire.back() >= 'a' ? "response" : "request"}};
}
class Reporter {
  std::string epoch_;
  std::uint64_t sequence_ = 0, generated_ = 0;
  std::deque<Json> events_;

public:
  explicit Reporter(std::string epoch) : epoch_(std::move(epoch)) {}
  std::string datagram(int stream) {
    std::ostringstream s;
    s << "GLM1" << epoch_ << std::hex << std::setw(16) << std::setfill('0') << ++generated_
      << (stream ? 'B' : 'A');
    return s.str();
  }
  void observe(std::string_view wire, const char *kind) {
    auto id = identifier(wire);
    if (id.is_null())
      return;
    id["sequence"] = std::to_string(++sequence_);
    id["kind"] = kind;
    id["payloadLength"] = std::to_string(wire.size());
    id["timestampMonotonicNs"] =
        std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count());
    events_.push_back(id);
    if (events_.size() > 8)
      events_.pop_front();
  }
  Json report() const {
    return {{"apiVersion", "graphlab.message-observations/v1"},
            {"epoch", epoch_},
            {"clock", "reporter-monotonic-ns"},
            {"payloadRecorded", false},
            {"total", std::to_string(sequence_)},
            {"evicted", std::to_string(sequence_ - events_.size())},
            {"events", events_}};
  }
};
} // namespace lab_support::messages
