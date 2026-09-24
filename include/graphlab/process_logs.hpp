#pragma once
#include <graphlab/runtime.hpp>
namespace graphlab::process_logs {
using runtime::Json;
// Immutable bounded tail snapshots, not a lossless event stream.
class History {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit History(const std::filesystem::path &);
  ~History();
  void start(runtime::Backend &, std::function<Json()> candidates);
  void ingest(const Json &context, const Json &snapshot, std::int64_t now = 0);
  Json query(const Json &params);
  Json download(const Json &params);
};
} // namespace graphlab::process_logs
