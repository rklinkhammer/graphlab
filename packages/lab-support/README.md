# LabSupport 1.0.0

C++23 contracts, bounded YAML/JSON parsing, workload validation, topology validation, and SHA-256 canonical document identity. No subprocesses, Docker API, runtime mutation, or environment-owned wiring are included.

```sh
cmake -S packages/lab-support -B build/support -DCMAKE_BUILD_TYPE=Release
cmake --build build/support
cmake --install build/support --prefix "$PWD/build/support-install"
```

Independent consumers use:

```cmake
find_package(LabSupport 1.0.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_node PRIVATE LabSupport::contracts)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/path/to/support-install`. The install includes the pinned nlohmann/json and yaml-cpp CMake packages. The consuming build also needs OpenSSL 3 development files. The package test installs the package and builds two consumers without `add_subdirectory` or includes into the Graphlab source tree.

Public entry points in `lab_support/contracts.hpp` return `std::expected<T, Error>` for parsing and validation. `digest()` uses OpenSSL SHA-256 and throws only on a crypto/serialization failure. `read_document()` reads at most 1 MiB plus one limit-check byte. Prefer `validate()` over manually constructing `ValidatedTopology`; the planner revalidates its raw inputs before producing a plan.

`dependencies.lock.json` is the authoritative archive/version/hash source consumed by CMake. Exact package references are required for application releases. CMake FetchContent supports offline source overrides (`FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON`, `FETCHCONTENT_SOURCE_DIR_YAML-CPP`); those overrides bypass archive verification, so only use preverified source trees.

Source/API versioning and serialized contract versions are separate. This is the initial package API, with no cross-compiler C++ ABI compatibility promise. Pin toolchain and runtime dependencies when packaging binary libraries. Independent Git publication and package-registry distribution are future release operations; M0 creates neither remote repositories nor published artifacts.
# M2 lifecycle package

The installed package additionally exports `LabSupport::lifecycle`. Its public `lab_support/lifecycle.hpp` provides `run_node(argc, argv, application)` for the C++ fixture gate and data-interface-only UDP echo/probe service. Nodes start held and accept `control status`, `control release`, `control quiesce`, and `control probe IPV4` through a private in-container Unix socket. This is fixture protocol `graphlab.gate/v1`, not a generic application process supervisor or an M3 release lease.

Both independently built [Docker nodes](../../docker-nodes/README.md) link this target without sibling source access. See [M2](../../docs/m2-executor.md) for target-ISA builds and runtime boundaries.
