# Graphlab

M0 implements a C++23 topology validator and deterministic dry-run planner. M1 adds a C++23 read-only agent/API and an authenticated React topology console. Neither milestone starts Docker containers, QEMU guests or OVS bridges.

M2 adds opt-in durable Linux execution, Docker/shared-OVS resources, run/job controls and shared C++ gated nodes. See [M2 setup and qualification limits](docs/m2-executor.md). The original agent invocation remains read-only; execution requires explicit state/peer configuration.

M3 adds capture-first runs, independent C++ PCAPNG workers, renewable traffic leases and verified artifact downloads. See [M3 setup and behavior](docs/m3-captures.md) and [verification evidence](docs/validation/m3-verification.md). No-capture development topologies still require explicit acknowledgement.

M4 adds QEMU guests and recorded Docker/SSH/serial consoles. See [operations](docs/m4-qemu-consoles.md), [preserved PowerPC/TCG and ARM64/KVM templates](qemu-guests/README.md), and [verification](docs/validation/m4-verification.md).

M7 adds [direct guest edges and an optional C++ QEMU container runner](docs/m7-backends.md). Experimental namespace OVS remains rejected until independent ownership and failure boundaries are qualified.

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

[Packet history v1](docs/packet-history.md) indexes finalized, checksum-verified PCAPNG segments in a bounded, persistent metadata store. The console filters by selected node/edge and protocol, freezes older pages, and links each packet to its exact capture artifact and block offset. Active files, inferred packet direction and application-message correlation are excluded.

Packet-index maintenance now recycles the bounded segment catalog without automatically replaying retired captures. The console provides run-level rebuild and explicitly scoped database recovery, with preserved capture files and a quarantined prior index. See [recovery and limits](docs/packet-history.md#recovery-procedure-and-failure-behavior).

Install the exact frontend and Playwright versions from the lockfile, install the matching Chromium build, and build the current console:

```sh
npm ci --prefix console/web
npx --prefix console/web playwright install chromium
npm run build --prefix console/web
```

Create a private runtime directory and operator credential from the repository root:

```sh
mkdir -m 700 build/m1-runtime
build/dev/lab-api init-auth build/m1-runtime/auth.json
```

The command prints the credential once. Start the read-only agent in one terminal:

```sh
build/dev/lab-agent \
  --socket "$PWD/build/m1-runtime/agent.sock" \
  --topologies "$PWD/topologies" \
  --lock "$PWD/topologies/artifacts.lock.json"
```

Start the API and static console in another terminal:

```sh
build/dev/lab-api \
  --socket "$PWD/build/m1-runtime/agent.sock" \
  --auth "$PWD/build/m1-runtime/auth.json" \
  --assets "$PWD/console/web/dist" \
  --port 8088
```

Open [http://127.0.0.1:8088](http://127.0.0.1:8088) and sign in with the generated credential. Use `127.0.0.1`, not `localhost`, because the API validates the browser authority. This profile is read-only; Linux execution and later runtime features require the milestone-specific agent configuration described in [M2 setup](docs/m2-executor.md), [M3 captures](docs/m3-captures.md), and [M4 QEMU consoles](docs/m4-qemu-consoles.md). See [M1 console setup and invocation](docs/m1-console.md) for deployment and security details.

To exercise the real agent, API, and console in a browser that remains visible while the test harness drives it, run:

```sh
npm run test:headed --prefix console/web -- tests/console.spec.ts
```

The harness uses port 18088, creates a temporary credential, opens Chromium, tests the console, and cleans up its child services and private data. A graphical desktop session is required. The regular `npm test --prefix console/web` command remains headless for automation.

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

`packages/lab-support` is a standalone CMake project with its own version and dependency lock. It exports `LabSupport::contracts` through `find_package(LabSupport 1.0.0 EXACT CONFIG REQUIRED)`; see [package instructions](packages/lab-support/README.md). The package currently bootstraps in this repository; it is not claimed to have a separate Git history or published release. It can be built/released independently without the planner or CLI. No privileged controller code is in the package.

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
