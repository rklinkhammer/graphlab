#pragma once
#include <graphlab/capture.hpp>
namespace graphlab::terminal {
using runtime::Json;
using Clock = std::chrono::steady_clock;
class Lease {
  std::string owner_, token_;
  Clock::time_point until_{};

public:
  Json acquire(const std::string &owner, bool takeover, Clock::time_point now = Clock::now());
  void require(const std::string &owner, const std::string &token,
               Clock::time_point now = Clock::now()) const;
  void renew(const std::string &owner, const std::string &token,
             Clock::time_point now = Clock::now());
  void revoke();
};
class Recording {
  int fd_ = -1;
  std::filesystem::path path_;
  std::uint64_t size_ = 0, sequence_ = 0, output_ = 0, budget_;
  Clock::time_point anchor_ = Clock::now();

public:
  Recording(const std::filesystem::path &, std::uint64_t budget);
  ~Recording();
  void append(std::uint32_t type, std::string_view bytes);
  void sync();
  void close();
  std::uint64_t offset() const { return output_; }
  std::uint64_t sequence() const { return sequence_; }
};
Json replay(const std::filesystem::path &, std::uint64_t sequence);
std::string encode(std::string_view);
std::string decode(const std::string &);
int connect_unix(const std::filesystem::path &);
Json request(const Json &descriptor, const std::string &operation, const std::string &generation,
             Json params = Json::object());
Json descriptor(const Json &run, const std::string &node, const std::filesystem::path &root,
                const std::string &kind);
void launch(const Json &config);
void cleanup_docker(const Json &config);
void stop(const Json &config, const std::string &generation);
Json docker_session(const Json &run, const Json &resource, const std::filesystem::path &root,
                    bool record_input);
void docker_attach(const Json &descriptor, const std::string &generation);
Json ssh_session(const Json &run, const Json &resource, const std::filesystem::path &root,
                 bool record_input);
void docker_resize(const Json &descriptor, unsigned rows, unsigned columns);
Json artifacts(const Json &run);
Json download(const Json &run, const std::string &id, std::uint64_t offset);
} // namespace graphlab::terminal
