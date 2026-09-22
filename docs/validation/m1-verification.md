# M1 verification — 2026-09-21

M1 implements a read-only console with C++23 agent/API services, private Unix RPC, shared contract validation, authenticated HTTP reads and a React/TypeScript graph. Runtime observations remain explicitly unknown because M1 has no executor-owned mappings. This is a local development profile; it does not qualify the later privileged deployment.

## Independent verification rerun

Reverified the current workspace on 2026-09-21 after the implementation turn. Reconfigured/rebuilt the macOS and Linux ARM64 development builds, reran all CTest suites, rebuilt and tested the production browser bundle, and reran the sanitizer suites. All results in the table below passed again; linked logs were refreshed. No source changes were required by this verification.

| M1 exit criterion | Verification |
|---|---|
| C++ API/agent read surface and diagnostics | Real HTTP and Unix-socket child-process tests on both platforms |
| Variable graphs, disconnected nodes and parallel edges | Eight saved fixtures through inventory and Chromium; additional layout sizes 1–100 |
| Missing runtime mappings marked unknown | Null identities/observation timestamps checked in C++; unknown state visible in browser |
| Shared OVS domain visible | Inventory failure-domain metadata and rendered console inspected |
| Auth/Origin/peer checks | Credential, session, expiry, revocation, Host/Origin, CSRF and actual socket peer-UID comparisons pass |

Reviewed catalog/inventory, RPC framing and peer checks, HTTP routing/authentication, graph construction and browser state handling. HTTP routing currently performs a synchronous, three-second-bounded RPC on its event-loop thread; a stalled agent can delay other HTTP work. The five-second transport timers are not a guarantee of five-second total response latency under queued requests. Concurrent-load responsiveness is not qualified by these acceptance tests.

| Check | Evidence/result |
|---|---|
| macOS ARM64 / AppleClang 21 / Boost 1.92.0 | All 7 CTest suites passed, including 79 M1 checks and all 56 M0 acceptance cases |
| Linux ARM64 / Ubuntu 24.04 / GCC 13.3 / Boost 1.92.0 | All 7 CTest suites passed with real Linux Unix sockets and HTTP child processes |
| macOS AddressSanitizer + UndefinedBehaviorSanitizer | All 6 selected suites passed, including M1; no sanitizer diagnostics |
| Browser production bundle | TypeScript check and Vite build passed; dependency install audited with no reported vulnerabilities |
| Chromium / Playwright | Four tests cover all eight graph fixtures, graph sizes 1–100 in layout tests, reversed parallel edges/self-links, management toggle, inspector, login/reload/logout, invalidated sessions and agent outage |
| Visual inspection | Checked rendered console screenshot; long same-row links routed above intervening nodes |

Logs: [macOS CTest](m1-macos-ctest.txt), [Linux ARM64 CTest](m1-linux-arm64-ctest.txt), [sanitizer CTest](m1-macos-sanitizer-ctest.txt), [browser tests](m1-browser-tests.txt). [Console screenshot](m1-console.png).

C++ tests exercise all saved graph inventories, missing identities/freshness, unsupported/malformed RPC envelopes, actual peer credential comparisons, oversized/idle Unix frames, HTTP body limits, credential verification, login rate limits, hostile Host/Origin, duplicate Host, session cookie attributes, CSRF, expiry/revocation, traversal, read-only method enforcement, agent-unavailable behavior and orderly socket cleanup. They use temporary private directories and terminate their child services. The sanitizer run excludes the separately configured installed-package consumers, which pass in the normal builds on both platforms.

Linux exposed AppleDouble metadata sidecars in the copied topology directory. Catalog discovery now ignores hidden metadata files, with a regression check. End-to-end tests also caught an inventory URL prefix error, corrected before final runs. Browser tests respect the actual login rate limit between independent sessions.

## Environment and limits

The Linux build reused the temporary extracted CMake/Ninja/OpenSSL development tools documented for M0. Boost 1.92.0 headers and header-only CMake configuration were copied from the observed Homebrew package into `/tmp/graphlab-m0-build-tools/boost`; no compiled macOS libraries were used on Linux. System packages, Docker resources, OVS networking and guest lifecycles were not changed. Boost is exact-version constrained, but supplied through the host package manager rather than a checksum-pinned source archive. Browser dependencies are integrity-locked by npm.

The browser build emits an upstream React Flow `use client` bundler warning; this is a client-only application and browser execution passed. Node 26.8.1 and Chromium 153.0.8010.12 were used. Browser rendering was tested on macOS; Linux evidence covers C++ services and protocols.

Still unqualified: GCC 14/libstdc++ 14 and Linux x86-64. M1 does not implement real Docker/OVS/QEMU observations, owned resource discovery, privileged/separate-UID deployment, captures, terminals, telemetry, durable jobs or TLS/multi-user operation. Unknown runtime states and synthetic sample artifact references remain visible, rather than being promoted to successful runtime evidence.

Launch instructions and bounded protocol details: [M1 console](../m1-console.md).
