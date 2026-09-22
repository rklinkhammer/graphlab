#pragma once
#include <condition_variable>
#include <functional>
#include <graphlab/console.hpp>
#include <mutex>
#include <thread>
struct sqlite3;
namespace graphlab::runtime {
using lab_support::Json;
struct Failure : std::runtime_error {
  int status;
  Failure(std::string code, int value = 422) : std::runtime_error(std::move(code)), status(value) {}
};
struct ProcessResult {
  int code;
  std::string output;
};
ProcessResult process(const std::vector<std::string> &argv, int timeout_seconds = 10);
namespace detail {
Json lookup_link(const ProcessResult &, const std::string &name);
bool lookup_present(const ProcessResult &);
void verify_namespace(const Json &expected, const Json &container, std::uint64_t inode);
} // namespace detail
struct DockerResponse {
  unsigned status;
  std::string body;
};
DockerResponse docker_request(const std::string &method, const std::string &path,
                              const Json &body = nullptr);
Json docker_json(const std::string &method, const std::string &path, const Json &body = nullptr);
class Backend {
public:
  virtual ~Backend() = default;
  virtual void configure(const std::filesystem::path &) {}
  virtual Json capture_plan(const Json &) { throw Failure("capture_backend_unavailable"); }
  virtual Json capture_control(const Json &, const std::string &) {
    throw Failure("capture_backend_unavailable");
  }
  virtual void preflight(const Json &topology, const Json &artifacts) = 0;
  virtual Json prepare(const Json &run, const Json &resource) = 0;
  virtual void remove(const Json &run, const Json &resource) = 0;
  virtual void activate(const Json &run) = 0;
  virtual void gate(const Json &run, const std::string &action) = 0;
  virtual Json observe(const Json &run) = 0;
};
class LinuxBackend final : public Backend {
  std::filesystem::path capture_root_;
  std::filesystem::path state_root_;

public:
  void configure(const std::filesystem::path &p) override { capture_root_ = p / "captures"; state_root_ = p; }
  Json capture_plan(const Json &) override;
  Json capture_control(const Json &, const std::string &) override;
  void preflight(const Json &, const Json &) override;
  Json prepare(const Json &, const Json &) override;
  void remove(const Json &, const Json &) override;
  void activate(const Json &) override;
  void gate(const Json &, const std::string &) override;
  Json observe(const Json &) override;
};
class Engine {
  sqlite3 *db_ = nullptr;
  int lock_ = -1;
  Backend &backend_;
  const console::Catalog &catalog_;
  std::filesystem::path directory_;
  Json state_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  std::thread worker_;
  void save();
  void work();
  void execute(const std::string &job);
  void cleanup(const std::string &run);
  void close_captures(const std::string &id);
  void monitor();

public:
  Engine(const std::filesystem::path &directory, Backend &, const console::Catalog &);
  ~Engine();
  Json dispatch(const Json &request, uid_t principal);
  Json inventory(Json logical);
};
std::vector<Json> resources(const Json &run);
std::string resource_name(const Json &run, const std::string &key);
} // namespace graphlab::runtime
