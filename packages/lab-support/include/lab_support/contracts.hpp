#pragma once
#include <expected>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace lab_support {
using Json = nlohmann::json;
struct Error {
  std::string code, path, message;
};
template <class T> using Result = std::expected<T, Error>;
// Bounded YAML/JSON decoder. No I/O; rejects aliases, tags, duplicates and multiple documents.
Result<Json> parse_document(std::string_view text);
Result<std::string> read_document(const std::string &path);
std::string digest(const Json &value);
std::string crypto_version();
struct Port {
  std::string role, medium;
  int mtu;
  Json vlan;
};
struct Node {
  std::string kind, workload;
  std::map<std::string, Port> ports;
};
struct Edge {
  std::string id, first, second;
};
struct ValidatedTopology {
  Json canonical;
  Json artifacts;
  std::map<std::string, Node> nodes;
  std::vector<Edge> edges;
  std::string hash;
};
// Both inputs are caller-provided values; artifact hashes are checked locally, never fetched.
Result<ValidatedTopology> validate(const Json &topology, const Json &artifact_lock);
Result<Json> validate_workload(const Json &contract);
} // namespace lab_support
