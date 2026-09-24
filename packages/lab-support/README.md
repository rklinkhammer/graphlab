# LabSupport 1.4.0

C++23 contracts, bounded YAML/JSON parsing, workload validation, topology validation, and SHA-256 canonical document identity. No subprocesses, Docker API, runtime mutation, or environment-owned wiring are included.

```sh
cmake -S packages/lab-support -B build/support -DCMAKE_BUILD_TYPE=Release
cmake --build build/support
cmake --install build/support --prefix "$PWD/build/support-install"
```

Independent consumers use:

```cmake
find_package(LabSupport 1.4.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_node PRIVATE LabSupport::contracts)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/path/to/support-install`. The install includes the pinned nlohmann/json and yaml-cpp CMake packages. The consuming build also needs OpenSSL 3 development files. The package test installs the package and builds two consumers without `add_subdirectory` or includes into the Graphlab source tree.

Public entry points in `lab_support/contracts.hpp` return `std::expected<T, Error>` for parsing and validation. `digest()` uses OpenSSL SHA-256 and throws only on a crypto/serialization failure. `read_document()` reads at most 1 MiB plus one limit-check byte. Prefer `validate()` over manually constructing `ValidatedTopology`; the planner revalidates its raw inputs before producing a plan.

`dependencies.lock.json` is the authoritative archive/version/hash source consumed by CMake. Exact package references are required for application releases. CMake FetchContent supports offline source overrides (`FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON`, `FETCHCONTENT_SOURCE_DIR_YAML-CPP`); those overrides bypass archive verification, so only use preverified source trees.

Source/API versioning and serialized contract versions are separate. This is the initial package API, with no cross-compiler C++ ABI compatibility promise. Pin toolchain and runtime dependencies when packaging binary libraries. Independent Git publication and package-registry distribution are future release operations; M0 creates neither remote repositories nor published artifacts.
# M2 lifecycle package

The installed package additionally exports `LabSupport::lifecycle`. Its public `lab_support/lifecycle.hpp` provides `run_node(argc, argv, application)` for the C++ fixture gate and data-interface-only UDP echo/probe service. Nodes start held and accept `control status`, `control release`, `control quiesce`, and `control probe IPV4` through a private in-container Unix socket. This is fixture protocol `graphlab.gate/v1`, not a generic application process supervisor or an M3 release lease.

Both independently built [Docker nodes](../../docker-nodes/README.md) link this target without sibling source access. See [M2](../../docs/m2-executor.md) for target-ISA builds and runtime boundaries.

M3 adds `release-lease` and `renew-lease` to the shared gate protocol. Leases use a monotonic ten-second deadline; expiry holds the gate and closes the UDP fixture socket. Renewal cannot reopen an expired lease. Existing `release` remains available only for explicitly selected no-capture development runs. See [M3 capture and lease qualification](../../docs/m3-captures.md).

## M5 telemetry package

`LabSupport::telemetry` exports `lab_support/telemetry.hpp`: pure C++23 directional rate and counter-epoch derivation over typed JSON observations. It preserves uint64 counters as decimal strings, uses monotonic deltas, and emits null rates at resets or observation gaps. It links the shared contracts package and contains no Linux collector, SQLite store or privileged fault executor. See [M5 operations](../../docs/m5-telemetry-faults.md) for the observation contract and limits.

The M6 qualification bundle retains installed 1.0.0 and 1.1.0 packages alongside independently compiled node images and source contexts. The [qualification manifest](../../qualification/README.md) binds actual archive bytes; a version alone is not a substitute for artifact identity.

## Gate protocol minors

SDK 1.1.0 adds optional `protocolMinor: 1` and `capabilities` metadata to `graphlab.gate/v1`; the existing commands and fields are unchanged. SDK 1.0.0 omits the minor, which means 0. `gate_protocol_minor()` accepts the qualified minors 0 and 1, ignores optional additions and rejects unsupported majors, minors or required features. The controller checks every Docker peer before releasing any node and validates release/renew acknowledgements. See the [declared compatibility matrix](../../qualification/protocol-minor-matrix.md). These are retained local qualification releases, not published registry artifacts or a cross-compiler ABI promise.

SDK 1.2.0 adds validation of immutable QEMU `vm.runnerImage` IDs and direct guest attachment graphs for M7. The gate wire protocol remains 1.1. Frozen M6 SDK/image archives are unchanged.

SDK 1.3.0 adds optional [application telemetry v1](../../docs/application-telemetry.md). `LabSupport::telemetry` exports strict report validation and rate/histogram derivation via `lab_support/application_telemetry.hpp`. The lifecycle overload `run_node(argc, argv, application, true)` instruments the UDP echo fixture; the original three-argument API remains uninstrumented. Gate wire version and required capabilities are unchanged.

SDK 1.4.0 adds optional endpoint-owned [application edge reports](../../docs/application-edge-telemetry.md), preserving the v1 report and lifecycle overloads. A five-argument lifecycle overload enables the separate two-stream TCP qualification fixture; ordinary app-a remains the UDP fixture.
