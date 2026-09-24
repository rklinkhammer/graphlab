#pragma once
#include <graphlab/runtime.hpp>
#include <memory>
#include <span>
namespace graphlab::packets {
using runtime::Json;
// Pure decoder for the single-interface, little-endian Ethernet PCAPNG writer profile.
Json decode(std::span<const unsigned char>, const Json &context, std::size_t limit = 2000);
class History {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit History(const std::filesystem::path &);
  ~History();
  Json query(const Json &run, const Json &params);
  Json rebuild(const Json &run);
  // Caller must stop all index users first. Captures and the execution journal are untouched.
  static void recover(const std::filesystem::path &);
};
} // namespace graphlab::packets
