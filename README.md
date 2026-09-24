# Graphlab

Graphlab validates topology definitions and runs Linux network experiments through an authenticated web console. The execution profile provides Docker/OVS and QEMU lifecycle controls, recorded consoles, interface metrics, capture artifacts, packet history and optional workload telemetry/message observations.

## Run the web console for a topology

**Follow [Run the console for a specific topology](docs/run-console.md).** It gives a complete `console-triangle` example with real Docker images, a matching artifact lock, capture-first execution and generated message traffic:

1. Prepare a Linux host with Docker, OVS and systemd.
2. Build/install the controller, capture/terminal workers and current frontend.
3. Build the two workload images and create the topology's verified artifact lock.
4. Create the operator credential and private state/socket directories.
5. Launch the root agent with **`--state` and `--allow-uid`**, then the unprivileged API with **`--agent-uid 0`**.
6. Verify `execution: true`, open `http://127.0.0.1:8088`, select the topology and click **Start selected topology**.
7. Inspect the run, quiesce to finalize captures, then destroy its runtime resources.

The guide includes exact commands, a same-port SSH tunnel for macOS/remote browsers, a smaller capture policy, existing-topology substitution and troubleshooting. Run the executor on Linux; building on macOS provides the read-only tools, not Linux execution.

**Why an older launch shows only M0/M1:** invoking `lab-agent` without `--state` and `--allow-uid` intentionally starts its read-only profile. Updating the frontend does not enable execution. Also, the checked-in `topologies/` lock uses synthetic artifact identities; those examples cannot be started unchanged. The [read-only setup](docs/m1-console.md) remains available specifically for viewing definitions.

## Build and test

Requirements: CMake 3.28+, Ninja, a C++23 compiler/standard library with `std::expected`, Boost 1.92.0 headers/CMake configuration, OpenSSL 3 and SQLite 3.24+ development headers/libraries. Linux builds also require libpcap development files; M3 execution requires systemd. The build fetches checksum-pinned yaml-cpp 0.8.0 and nlohmann/json 3.12.0 archives. The optional browser build uses Node/npm. Python is not required.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On Linux with GCC 14 installed, use the `linux-gcc14` configure/build/test preset instead. Other toolchains must pass the feature probe and tests. On macOS, OpenSSL installed under a nonstandard prefix can be selected using `-DOPENSSL_ROOT_DIR=/path/to/openssl` when configuring.

## Web console

The console connects topology selection to serial/SSH/container sessions, searchable process logs, directional link metrics, capture artifacts, and event history. See [feature parity and verification](docs/web-console-feature-parity.md) for implemented features and remaining GraphX-specific gaps.

[Application telemetry v1](docs/application-telemetry.md) adds optional workload message/payload rates, errors, rejection/backpressure counters and explicitly defined local service-time histograms. The instrumented C++ `app-a` fixture reports through the existing gate; bounded agent history and authenticated console views remain separate from interface counters. Uninstrumented workloads remain supported.

[Packet history v1](docs/packet-history.md) indexes finalized, checksum-verified PCAPNG segments in a bounded, persistent metadata store. The console filters by selected node/edge and protocol, freezes older pages, and links each packet to its exact capture artifact and block offset. Active files and inferred packet direction are excluded. Separate opt-in [GLM1 message correlation](docs/message-observations.md) verifies identifiers in retained capture bytes.

Packet-index maintenance now recycles the bounded segment catalog without automatically replaying retired captures. The console provides run-level rebuild and explicitly scoped database recovery, with preserved capture files and a quarantined prior index. See [recovery and limits](docs/packet-history.md#recovery-procedure-and-failure-behavior).

For operational startup commands, use the [step-by-step execution guide](docs/run-console.md), rather than the historical milestone profiles. Browser automation is optional: `npm test --prefix console/web` runs the default tests; `npm run test:headed --prefix console/web -- tests/console.spec.ts` runs the read-only console test in a visible browser after installing Playwright's Chromium. That test starts its own temporary read-only services, not your experiment.

## Invoke M0

```sh
build/dev/lab preflight
build/dev/lab validate topologies/triangle.yaml --lock topologies/artifacts.lock.json
build/dev/lab plan topologies/triangle.yaml --lock topologies/artifacts.lock.json
build/dev/lab workload topologies/app-a.workload.json
```

`validate` checks topology semantics and the local artifact/contract hash chain. `plan` emits JSON describing dependency-ordered resource preparation, per-edge captures, release barriers, and resource teardown order. It always declares `executable: false`; physical names, runtime ownership, artifact availability and host suitability need later execution stages. Output goes to stdout; errors are structured JSON on stderr. Exit codes: 0 success, 1 usage error, 2 invalid input or runtime inspection error.

The included lock contains **synthetic test identities**, including `registry.invalid` images. They are not deployable artifacts. M0 verifies the embedded contract and lock hashes and requires immutable-looking binary references, but does not fetch or verify image/disk bytes. Do not interpret valid fixture metadata as a successful registry or VM check.

Additional configuration examples: `chain.yaml`, `star.yaml`, `ring.yaml`, `mesh.yaml`, `disconnected.yaml`, `isolated.yaml`, and `parallel.yaml`, all under `topologies/`. They use the same lock.

To update your own lock, first recompute each changed embedded contract's `contractSha256`, then set the topology's `artifactLock` to the lock digest:

```sh
build/dev/lab hash path/to/workload-contract.json
build/dev/lab hash path/to/artifacts.lock.json
```

Hashing uses parsed JSON with lexically sorted object keys, compact serialization, and SHA-256. Array order is significant for artifact/contract hashing. Topology hashing additionally sorts edges, management attachments and trunk VLAN lists and fills documented defaults. Endpoint order remains significant because it defines direction.

## Common contract package

`packages/lab-support` is a standalone CMake project with its own version and dependency lock. It exports `LabSupport::contracts` through `find_package(LabSupport 1.6.0 EXACT CONFIG REQUIRED)`; see [package instructions](packages/lab-support/README.md). The package currently bootstraps in this repository; it is not claimed to have a separate Git history or published release. It can be built/released independently without the planner or CLI. No privileged controller code is in the package.

`docker-nodes/` now contains independently buildable M2 C++ fixture applications. No separate node Git repositories or published images have been created; local Linux test images were built for qualification.

See [input contract](schemas/README.md), [M0 status and validation](docs/cpp23-status.md), and the [full implementation plan](docs/option-d-console-plan.md).

M5 adds directional telemetry, bounded history, typed netem faults and a correlated timeline. See [operation and supported placements](docs/m5-telemetry-faults.md) and [verification](docs/validation/m5-verification.md).

M6 adds [operational qualification tooling](docs/m6-qualification.md), explicit T01–T19 release gates, retained artifact hashes and baseline/telemetry/capture capacity measurements. Incomplete gates prevent a full qualification claim.

Application/network console views: [application dataflow mapping contract and example](docs/application-dataflow.md).

[Application-edge telemetry](docs/application-edge-telemetry.md) adds endpoint-owned stream reports, declared protocol metadata, and a separate two-stream TCP qualification fixture in `docker-nodes/app-streams`. Existing node telemetry remains compatible; source-only controls and message correlation are separate increments.

Source-specific pause/resume is documented in [Source controls v1](docs/source-controls.md), including capability/identity requirements, run-quiesce precedence and bounded command retention.

Retained process logs: [snapshot contract, limits and qualification](docs/process-logs.md).
Capture and diagnostic inspectors: [field provenance, metadata and limitations](docs/capture-inspectors.md).

Opt-in message history and verified capture correlation: [contract, bounds and qualification](docs/message-observations.md).
