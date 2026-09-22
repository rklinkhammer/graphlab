# M0 verification — 2026-09-21

Result: available implementation checks pass. Full platform qualification remains open for GCC 14/libstdc++ 14 and Linux x86-64. No implementation changes were required by this verification.

## Checks rerun

| Check | Result |
|---|---|
| macOS ARM64 development configure/build/CTest | 6/6 suites passed, including 56 C++ acceptance cases and installed-package consumers |
| Linux ARM64 development configure/build/CTest | Current source snapshot verified with GCC 13.3; 6/6 suites passed |
| Fresh macOS AddressSanitizer + UndefinedBehaviorSanitizer build | 5/5 selected suites passed with no sanitizer diagnostics |
| Saved topology files | All eight YAML fixtures passed both CLI validation and planning: chain, disconnected, isolated, mesh, parallel, ring, star, triangle |
| x86-64 C++23 feature probe | AppleClang syntax-only `std::expected` probe passed; no Linux x86-64 execution evidence |
| Implementation review | Reviewed document parsing, contract validation, planner dependencies, CLI side effects, dependency locks and package-consumer checks |

The sanitizer run excludes `installed_package`: its separately configured consumers do not inherit sanitizer link flags. Package installation and both independent consumers passed in the normal builds on both platforms.

Evidence: [macOS suites](m0-macos-ctest.txt), [Linux ARM64 suites](m0-linux-arm64-ctest.txt), [macOS sanitizer suites](m0-macos-sanitizer-ctest.txt).

## Acceptance boundaries

- Shared C++23 contracts, malformed-input rejection, graph variants, deterministic resource ordering, and local contract/lock integrity pass their available tests.
- The CLI side-effect test verifies unchanged input hashes, no new files in its working directory, repeated identical plans, and rejection of `apply`. Source review found no executor, subprocess or daemon/network calls in planning. This is not a system-call trace of the entire process.
- YAML/JSON dependency archives are checksum pinned. OpenSSL is a system dependency with a minimum version and observed-version evidence; the complete host toolchain is not hermetically pinned.
- Reference GCC 14 and full Linux x86-64 builds still need appropriate runners. The available Linux VM has GCC 13.3.
- Privileged Docker/OVS/QEMU execution, real artifact verification, KVM access, capture activation/direction and crash cleanup require later Linux runtime milestones. The sample artifact identities are synthetic.

## Reproduction

Normal suites: `cmake --preset dev`, `cmake --build --preset dev`, `ctest --preset dev`.

The fresh sanitizer build used `build/verify`, Ninja, Debug, `BUILD_TESTING=ON`, and `CMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer`, with dependency source overrides pointing to the existing checksum-pinned downloads under `build/dev/_deps`. Run `ctest --test-dir build/verify --output-on-failure -E installed_package`.

Linux used the temporary extracted build tools and source directory documented in [C++23 status](../cpp23-status.md). No runtime resources or system packages were changed.
