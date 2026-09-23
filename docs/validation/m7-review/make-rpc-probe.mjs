import fs from 'node:fs';
let s = fs.readFileSync('tests/backends/linux.cpp', 'utf8');
s = s.replace('#include "../qemu/linux.cpp"', '#include "tests/qemu/linux.cpp"');
const helper = `
void rpcFault(runtime::Engine &engine, Json &run, Json &fault, bool apply) {
  run = call(engine, "run", {{"id", run["id"]}});
  Json params = {{"runId", run["id"]}, {"expectedRevision", run["revision"]},
                 {"idempotencyKey", console::random_hex(16)}};
  if (apply) {
    params["fault"] = {{"edge", fault["edge"]}, {"kind", "netem"},
                       {"direction", fault["direction"]}, {"lossPercent", 100},
                       {"durationSeconds", fault.value("reviewDuration", 60)}};
    auto admitted = call(engine, "fault.apply", params);
    fault["id"] = admitted["faultId"];
    auto job = wait(engine, admitted);
    check(job["state"] == "succeeded", "RPC fault apply journal: " + job.dump());
    run = call(engine, "run", {{"id", run["id"]}});
    fault["placement"] = run["faults"][fault["id"].get<std::string>()]["placement"];
  } else {
    params["faultId"] = fault["id"];
    auto job = wait(engine, call(engine, "fault.remove", params));
    check(job["state"] == "succeeded", "RPC fault removal journal: " + job.dump());
  }
}
`;
s = s.replace('int main(int argc, char **argv) {', helper + '\nint main(int argc, char **argv) {');
s = s.replaceAll('fault["placement"] = backend.fault(run, fault, "plan");', '');
s = s.replaceAll('backend.fault(run, fault, "apply");', 'rpcFault(engine, run, fault, true);');
s = s.replaceAll('backend.fault(run, fault, "remove");', 'rpcFault(engine, run, fault, false);');

s = s.replace('rpcFault(engine, run, fault, false);\n              std::this_thread', `if (cycle == 3) {
                bool expired = false;
                for (int attempt = 0; attempt < 300; ++attempt) {
                  auto current = call(engine, "run", {{"id", run["id"]}});
                  if (current["faults"][fault["id"].get<std::string>()]["state"] == "removed") {
                    expired = true;
                    break;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                check(expired, "RPC direct guest fault expires and journals removal");
              } else rpcFault(engine, run, fault, false);
              std::this_thread`);

s = s.replace('rpcFault(engine, run, fault, true);\n              std::this_thread', 'fault["reviewDuration"] = cycle == 3 ? 25 : 60;\n              rpcFault(engine, run, fault, true);\n              std::this_thread');
s = s.replace('attempt < 160', 'attempt < 300');
fs.writeFileSync('build/m7-review-rpc.cpp', s);
