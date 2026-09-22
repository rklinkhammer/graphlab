#pragma once
#include <graphlab/runtime.hpp>
#include <span>
namespace graphlab::capture {
using runtime::Json;
void atomic_json(const std::filesystem::path &, const Json &);
std::string file_hash(const std::filesystem::path &);
Json inspect_partial(const std::filesystem::path &);
class Writer {
  int fd_ = -1;
  std::filesystem::path directory_, partial_;
  Json config_, segments_ = Json::array();
  std::uint64_t bytes_ = 0, total_ = 0, packets_ = 0, sequence_ = 0;
  void block(std::uint32_t, std::span<const unsigned char>);

public:
  explicit Writer(std::filesystem::path directory, Json config);
  ~Writer();
  void open();
  void packet(std::uint64_t micros, std::span<const unsigned char>, std::uint32_t original);
  void sync();
  void close(Json stats);
  const Json &segments() const { return segments_; }
  std::uint64_t bytes() const { return bytes_; }
  std::uint64_t total() const { return total_ + bytes_; }
};
Json worker_call(const Json &capture, const std::string &operation, const std::string &generation,
                 const Json &params = Json::object());
Json plan(const Json &run, const std::filesystem::path &root);
Json control(const Json &run, const std::string &operation);
Json artifacts(const Json &run);
Json download(const Json &run, const std::string &id, std::uint64_t offset);
} // namespace graphlab::capture
