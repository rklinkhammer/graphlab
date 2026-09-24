#pragma once
#include <graphlab/runtime.hpp>
namespace graphlab::messages {
using runtime::Json;
Json validate(const Json &);
Json correlate(const Json &run, const Json &event, const Json &params);
class History {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit History(const std::filesystem::path &);
  ~History();
  void start(runtime::Backend &, std::function<Json()>);
  void ingest(const Json &context, const Json &report, std::int64_t now = 0);
  Json query(const Json &);
  Json event(const Json &);
};
} // namespace graphlab::messages
