#include <fstream>
#include <graphlab/runtime.hpp>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace graphlab::runtime;
struct Fake : Backend {
  void preflight(const Json &, const Json &) override {}
  Json prepare(const Json &, const Json &r) override { return {{"id", r["key"]}}; }
  void remove(const Json &, const Json &) override {}
  void activate(const Json &) override {}
  void gate(const Json &, const std::string &) override {}
  Json observe(const Json &) override { return Json::object(); }
  Json capture_plan(const Json &) override { return Json::array({{{"id", "test"}}}); }
  Json capture_control(const Json &, const std::string &a) override {
    return a == "stop" ? Json::array({{{"state", "interrupted"}, {"partialFilesPossible", true}}})
                       : Json::array();
  }
};
Json call(Engine &e, std::string m, Json p = Json::object()) {
  return e.dispatch({{"apiVersion", "graphlab.rpc/v1"}, {"method", m}, {"params", p}}, geteuid());
}
Json wait(Engine &e, Json a) {
  for (int i = 0; i < 1000; ++i) {
    auto j = call(e, "job", {{"id", a["jobId"]}});
    if (j["state"] != "queued" && j["state"] != "running")
      return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("timeout");
}
int main(int argc, char **argv) {
  char p[] = "/tmp/m3-review-XXXXXX";
  std::filesystem::path dir = mkdtemp(p);
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() { std::filesystem::remove_all(p); }
  } cleanup{dir};
  if (process({argv[2], "--fixtures", argv[1], dir.string(), "sha256:" + std::string(64, 'd')})
          .code)
    return 2;
  auto t = graphlab::console::load(dir / "m2.yaml");
  t["capture"]["required"] = true;
  std::ofstream(dir / "m2.yaml") << t.dump();
  graphlab::console::Catalog cat(dir, dir / "artifacts.lock.json");
  auto hash = lab_support::validate(t, graphlab::console::load(dir / "artifacts.lock.json"))->hash;
  auto state = dir / "state";
  std::filesystem::create_directory(state);
  chmod(state.c_str(), 0700);
  Fake f;
  Engine e(state, f, cat);
  auto a = call(e, "start", {{"topologyHash", hash}, {"idempotencyKey", "review-start"}});
  if (wait(e, a)["state"] != "succeeded")
    return 3;
  auto r = call(e, "run", {{"id", a["runId"]}});
  auto j = wait(e, call(e, "operate",
                        {{"runId", r["id"]},
                         {"expectedRevision", r["revision"]},
                         {"operation", "stop"},
                         {"idempotencyKey", "review-stop"}}));
  r = call(e, "run", {{"id", a["runId"]}});
  std::cout << Json{{"stopWorkerResult", "interrupted"},
                    {"jobState", j["state"]},
                    {"runState", r["state"]},
                    {"captureCoverage", r["captureCoverage"]}}
                   .dump(2)
            << std::endl;
  return r["captureCoverage"] == "closed" ? 1 : 0;
}
