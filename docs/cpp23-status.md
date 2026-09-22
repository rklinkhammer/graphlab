# C++23 implementation status

Updated 2026-09-21. Implements M0 from [option D](option-d-console-plan.md), with remaining platform qualification listed explicitly below.

M1 implementation is now available: [console setup](m1-console.md), [M1 verification](validation/m1-verification.md). The M0 evidence below remains specific to M0.

M2 adds the opt-in Linux executor and C++ fixture nodes: [setup and scope](m2-executor.md), [verification](validation/m2-verification.md).

M3 adds capture-first execution; M4 adds QEMU and recorded consoles. See [M4 operations](m4-qemu-consoles.md), [guest templates](../qemu-guests/README.md) and [M4 verification](validation/m4-verification.md). The historical M0-only scope below is unchanged.

## Implemented

- CMake/Ninja C++23 build, `std::expected` feature probe, development and reference GCC 14 presets, checksum-pinned YAML/JSON dependencies, system OpenSSL 3 hashing.
- Standalone, independently versioned `LabSupport 1.0.0` CMake package, exporting `LabSupport::contracts` for independent consumers. Its source currently lives under `packages/lab-support`; separate repository/publication has not been performed.
- Bounded YAML/JSON parsing with duplicate key, alias/tag, size/depth and ambiguous-document rejection.
- Workload and topology contracts, immutable-reference syntax checks, embedded contract/lock SHA-256 integrity, port/endpoint/VLAN/MTU/resource/management validation.
- Arbitrary graph handling: chain, star, ring, mesh, disconnected/isolated nodes, parallel links, multi-interface workloads and variable graph sizes. Unsupported endpoint backends fail explicitly.
- Deterministic resource-DAG planning, one capture per data edge, policy/capture/convergence/release ordering, QEMU TAP-before-start dependencies and reverse resource teardown order. No executor is linked.
- `lab preflight`, `hash`, `workload`, `validate`, and `plan` commands. Read-only preflight observes OS/compiler/tool paths/device access without launching probes or contacting daemons.
- Example topology files, a synthetic artifact lock, C++ acceptance cases and independent installed-package consumer tests. No Python source/runtime/test dependency.

The implemented input subset is specified in [schemas/README.md](../schemas/README.md). Later API/console/capture/runtime contracts from the design are not silently accepted as implemented fields.

## Validation actually run

Latest independent rerun: [M0 verification](validation/m0-verification.md), including a fresh macOS sanitizer build and validation/planning of all eight saved topology fixtures.

| Environment/check | Result |
|---|---|
| macOS ARM64, AppleClang 21, CMake 4.4.3, Ninja 1.13.2, OpenSSL 3.6.4 | Configure/build passed; all 6 CTest suites passed |
| Linux ARM64, Ubuntu 24.04, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1, OpenSSL 3.0.13 | Configure/build passed; all 6 CTest suites passed |
| C++ acceptance executable | 56 cases passed on both platforms; includes generated graphs with 1..20 switches in addition to named fixtures |
| Installed package consumption | Package installed; two independently configured consumer applications built and ran through `find_package(LabSupport 1.0.0 EXACT)` on both platforms |
| Standalone common-package Release build | Passed on macOS ARM64 |
| x86-64 C++23 compiler feature probe | AppleClang syntax-only `std::expected` probe passed; this is not a Linux x86-64 build/runtime qualification |
| CLI repeatability/no local file changes | Repeated plan byte-identical; topology/lock hashes unchanged; no new files in isolated CLI working directory; unsupported `apply` command rejected |

Evidence: [macOS CTest log](validation/m0-macos-ctest.txt), [Linux ARM64 CTest log](validation/m0-linux-arm64-ctest.txt), [macOS preflight](validation/m0-macos-preflight.json), [Linux preflight](validation/m0-linux-arm64-preflight.json), and [runtime inventory](../locks/runtime.json).

The Linux VM initially lacked CMake, Ninja and OpenSSL development headers. For the tests, Ubuntu packages were downloaded and extracted into `/tmp/graphlab-m0-build-tools`, without system package installation. A source snapshot was built under `/tmp/graphlab-m0-source`. The extracted OpenSSL headers were used with the existing system libcrypto; exact package versions are recorded. These temporary paths are test artifacts, not a service installation. No Docker workload, OVS bridge/interface, VM lifecycle, firewall rule or runtime package was changed.

Linux testing found a test-harness assumption that Make was installed; the installed-package test now uses the configured CMake generator. Code review also corrected the planner to prepare guest TAPs before starting paused QEMU and to place direct Docker–Docker capture inside a workload namespace. Both platforms were retested after those changes.

## Qualification still pending

M0 implementation and available tests are complete; the plan's full cross-platform exit matrix is not yet fully qualified:

1. Run `cmake --preset linux-gcc14`, build and test with GCC 14/libstdc++ 14. The available VM has GCC 13.3, which passes the required C++23 feature probe but is an alternative toolchain.
2. Run the full suite on Linux x86-64. Only macOS ARM64 and Linux ARM64 have full build/execution evidence.
3. Before later runtime milestones, verify real artifact bytes/registry access, exact VM image/firmware, host route conflicts, CPU/RAM budgets, Docker/OVS permissions, KVM access and ownership. `/dev/kvm` exists in the inspected VM, but the current nonprivileged user lacks read/write access. No permission changes were made.
4. The installed Containerlab reports `0.79.0-graphlab-arm64`, but patch provenance remains unverified. M0 does not use Containerlab.

Actual link/capture direction, activation acknowledgements, traffic leases, VLAN/RSTP failover, recording continuity and crash cleanup require Linux runtime implementations in later milestones. They are **not tested or promised by a dry-run plan**. The artifact lock included with the examples uses synthetic hashes and `registry.invalid` image names; metadata validation does not assert artifact availability.

## Invocation

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
build/dev/lab preflight
build/dev/lab validate topologies/triangle.yaml --lock topologies/artifacts.lock.json
build/dev/lab plan topologies/triangle.yaml --lock topologies/artifacts.lock.json
```

Plans print JSON to stdout and never apply resources. See the [README](../README.md) for package use and digest updates. M1 provides the read-only web console; deployment commands remain unimplemented.

## M3 capture execution

The C++23 implementation now includes independent libpcap/systemd workers, PCAPNG segments and manifests, capture barriers, shared node traffic leases and artifact access. See [M3 operation](m3-captures.md) and [M3 verification](validation/m3-verification.md) for tested scope and remaining qualification limits.

## M5 telemetry and faults

M5 adds shared C++ rate derivation, Linux directional counters, bounded SQLite history, typed netem jobs with expiry/recovery, and browser charts/timeline. See [operations and placement limits](m5-telemetry-faults.md) and [verification](validation/m5-verification.md). Guest-originating TAP faults require an unimplemented IFB backend and are explicitly rejected.
