#pragma once
#include <array>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
namespace lab_support::detail {
// Bounded, process-lifetime idempotency ledger. Never evict a command and silently replay it.
class Sources {
  using Json = nlohmann::json;
  std::string epoch_;
  std::array<bool, 2> paused_{};
  std::map<std::string, Json> commands_;

public:
  explicit Sources(std::string epoch) : epoch_(std::move(epoch)) {}
  bool paused(int i) const { return paused_.at(i); }
  Json status(const std::array<std::uint64_t, 2> &sent) const {
    Json sources = Json::array();
    for (int i = 0; i < 2; ++i)
      sources.push_back({{"id", i == 0 ? "alpha" : "beta"},
                         {"state", paused_[i] ? "paused" : "running"},
                         {"generatedDatagrams", std::to_string(sent[i])}});
    return {{"apiVersion", "graphlab.source-control/v1"}, {"epoch", epoch_}, {"sources", sources}};
  }
  Json apply(const Json &p) {
    if (!p.is_object() || p.size() != 5 ||
        p.value("apiVersion", "") != "graphlab.source-control/v1")
      throw std::runtime_error("source_contract_invalid");
    for (auto k : {"requestId", "epoch", "source", "action"})
      if (!p.contains(k) || !p[k].is_string())
        throw std::runtime_error("source_contract_invalid");
    auto id = p["requestId"].get<std::string>();
    if (!std::regex_match(id, std::regex("[A-Za-z0-9_-]{1,64}")))
      throw std::runtime_error("source_request_id_invalid");
    if (p["epoch"] != epoch_)
      throw std::runtime_error("source_epoch_changed");
    auto source = p["source"].get<std::string>(), action = p["action"].get<std::string>();
    if ((source != "alpha" && source != "beta") || (action != "pause" && action != "resume"))
      throw std::runtime_error("source_control_unsupported");
    if (commands_.contains(id)) {
      if (commands_[id] != p)
        throw std::runtime_error("source_request_conflict");
    } else {
      if (commands_.size() >= 256)
        throw std::runtime_error("source_command_capacity");
      paused_[source == "alpha" ? 0 : 1] = action == "pause";
      commands_[id] = p;
    }
    auto out = p;
    out["outcome"] = "acknowledged";
    return out;
  }
};
} // namespace lab_support::detail
