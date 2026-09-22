# Graphlab

M0 implements a C++23 topology validator and deterministic dry-run planner. M1 adds a C++23 read-only agent/API and an authenticated React topology console. Neither milestone starts Docker containers, QEMU guests or OVS bridges.

See [M1 console setup and invocation](docs/m1-console.md) for build, login, service startup and browser instructions.

M2 adds opt-in durable Linux execution, Docker/shared-OVS resources, run/job controls and shared C++ gated nodes. See [M2 setup and qualification limits](docs/m2-executor.md). The original agent invocation remains read-only; execution requires explicit state/peer configuration.

M3 adds capture-first runs, independent C++ PCAPNG workers, renewable traffic leases and verified artifact downloads. See [M3 setup and behavior](docs/m3-captures.md) and [verification evidence](docs/validation/m3-verification.md). No-capture development topologies still require explicit acknowledgement.

M4 adds QEMU guests and recorded Docker/SSH/serial consoles. See [operations](docs/m4-qemu-consoles.md), [preserved PowerPC/TCG and ARM64/KVM templates](qemu-guests/README.md), and [verification](docs/validation/m4-verification.md).

## Build and test

Requirements: CMake 3.28+, Ninja, a C++23 compiler/standard library with `std::expected`, Boost 1.92.0 headers/CMake configuration, OpenSSL 3 and SQLite 3.24+ development headers/libraries. Linux builds also require libpcap development files; M3 execution requires systemd. The build fetches checksum-pinned yaml-cpp 0.8.0 and nlohmann/json 3.12.0 archives. The optional browser build uses Node/npm. Python is not required.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On Linux with GCC 14 installed, use the `linux-gcc14` configure/build/test preset instead. Other toolchains must pass the feature probe and tests. On macOS, OpenSSL installed under a nonstandard prefix can be selected using `-DOPENSSL_ROOT_DIR=/path/to/openssl` when configuring.

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
