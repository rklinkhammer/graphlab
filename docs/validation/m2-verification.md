# M2 verification

M2 implements the opt-in development executor described in [the operating guide](../m2-executor.md). It does not provide required-capture coverage. The original verification and subsequent remediation are distinguished below. The full milestone's exhaustive side-effect interruption gate remains partially qualified: Backend mutation calls are covered, but not every individual kernel or Docker API sub-operation.

## Results

### Remediation

The two source-review findings below have been fixed:

- Link absence now requires a successful complete link inventory. OVS absence uses successful `--if-exists get` queries; daemon, command, timeout and namespace-entry failures propagate instead of being treated as missing resources. Failed removal leaves its durable record intact and the run reconciling; a later recovery can retry it.
- Resume and edge removal compare the opened namespace inode and container ID with the recorded identity, recheck the inspected PID, and retain the descriptor for namespace operations. Both endpoints are checked before edge removal; all edge namespaces are checked before activation. A mismatched identity fails without deleting the edge.

New standard regressions cover failed versus empty lookups, namespace mismatch, retained cleanup state and successful cleanup retry. Authenticated HTTP admission races the actual CLI through one Unix-socket agent; identical retries return one job, and competing mutations conflict through both clients. The backend is simulated in that transport test; the HTTP router is called directly rather than through a browser/TCP connection.

The live Linux harness checks altered saved namespace identities against actual containers and expands abrupt exits from 22 to 56: before/after all 28 Backend mutation calls across start, stop, resume and destroy, including every resource removal. Internal kernel/API sub-operations, actual namespace replacement races and real daemon-outage stress remain outside this qualification.

Remediation evidence: [macOS CTest](m2-remediation-macos.txt), [sanitizers](m2-remediation-sanitizers.txt), [Linux CTest](m2-remediation-linux-ctest.txt), [Linux runtime](m2-remediation-linux.txt). The prior browser UI evidence remains applicable; no frontend code changed in this remediation.

**Remediation results:** macOS and Linux CTest each passed 8/8, ASan/UBSan passed 7/7, and all 56 live mutation-boundary crash cases passed. Live VLAN/gate tests and mismatched-namespace checks also passed. Final Graphlab container/network queries and OVS bridge inventory were empty. Retained Linux evidence is `/tmp/graphlab-m2-live-noFP2R`. Detailed transport and cleanup regression output is preserved in the [macOS log](m2-remediation-macos-detail.txt) and [Linux log](m2-remediation-linux-detail.txt). The identified implementation defects are remediated; this does not certify every internal side-effect interruption or all namespace replacement races.

### Follow-up verification before remediation

The requested follow-up verification reran the current workspace: macOS CTest 8/8, ASan/UBSan 7/7, production UI build, four browser regressions, Linux ARM64 CTest 8/8, and the live Linux lifecycle/VLAN/recovery suite with all 22 abrupt-exit cases passed. See [fresh Linux execution log](m2-linux-reverification.txt). The live M2 browser flow below is retained evidence from implementation, not a fresh run in this follow-up. Final Docker label queries and OVS bridge inventory were empty. No runtime implementation changes were made during this verification.

**Historical verdict: passing tested development profile, but M2's complete acceptance gate was not verified.** Source review found these correctness gaps, since addressed above:

1. **Cleanup treats lookup failures as absence.** In `cpp/runtime/linux.cpp`, `LinuxBackend::remove` returns for any nonzero `ovs-vsctl br-exists` result, including daemon/transport failures. The `link` helper likewise returns null for any failed `ip`/`nsenter` invocation. `Engine::cleanup` then durably marks the resource removed. An unavailable backend can therefore produce a false successful cleanup and leave resources behind. Lookup failures must be distinguished from confirmed absence, and failure cases must preserve reconciling state. This finding is from source review; an OVS outage was not injected into the shared VM.
2. **Stored namespace identity is not enforced.** Edge creation records `namespaceInode`, but there are no reads of that field in the runtime. Resume and removal reopen a namespace from the container's current PID without comparing it to the recorded identity. Link aliases/ifindexes and container labels provide other checks, but they do not establish the missing namespace identity guarantee. Add explicit checks and replaced-namespace tests before claiming this property.

The crash harness wraps resource `prepare` calls only. It does not interrupt activation, release/quiesce, removal, or each internal Docker/kernel side effect. Concurrent retries are tested at the engine boundary; simultaneous HTTP/CLI mutation conflict behavior has no dedicated live acceptance test. These coverage gaps prevent signing off the full milestone despite the passing suites.

| Check | Result | Evidence |
| --- | --- | --- |
| macOS ARM64 build and CTest | 8/8 passed | [CTest log](m2-macos-ctest.txt) |
| Linux ARM64 build and CTest | 8/8 passed | [CTest log](m2-linux-arm64-ctest.txt) |
| macOS ASan/UBSan | 7/7 passed; installed-package consumer excluded from this sanitizer run | [Sanitizer log](m2-macos-sanitizer-ctest.txt) |
| TypeScript and production UI build | Passed | `npm run build --prefix console/web` |
| Existing browser regressions | 4/4 passed | [Playwright log](m2-browser-regression.txt) |
| Browser controlling real Linux services | Start, quiesce, resume, destroy passed | [Playwright log](m2-browser-live.txt), [screenshot](m2-console.png) |
| Linux Docker/OVS lifecycle and recovery | Passed, including 22 abrupt-exit cases | [Runtime log](m2-linux-runtime.txt) |

The standard CTest suite includes the M0/M1 regressions and M2 journal tests. M2 checks state-directory exclusivity, idempotent replay, payload and stale-revision conflicts, eight concurrent same-key callers, lifecycle operations, cancellation with compensation, restart persistence and recovery with a fault-injecting backend. Standard CTest does not mutate Docker or host networking.

Live Linux tests used two independently compiled C++ nodes consuming the installed common package, three OVS switches in a ring, five data edges and a separate management network. Same-VLAN UDP traffic succeeded; different-VLAN traffic timed out. The management address did not expose the data echo service. Held nodes rejected probes, and quiesce/resume behavior was verified. RSTP converged before release. Each of the 11 resource preparation steps was interrupted before and after execution in a child process; restart fenced the run as reconciling, and explicit recovery removed its resources. An unrelated OVS bridge survived lifecycle teardown and every recovery case.

The live browser used an unprivileged API account without Docker-group membership, authenticated to a root agent over its group-accessible Unix socket. CLI recovery also exercised that root-agent RPC path. Concurrent admission is covered at the common engine boundary; a simultaneous live browser-versus-CLI stress run is not claimed.

## Environment and retained artifacts

- macOS ARM64: AppleClang 21, CMake 4.4.3, OpenSSL 3.6.4, SQLite 3.54.0, Boost 1.92.0.
- Linux VM: Ubuntu 24.04 ARM64, kernel 6.8, GCC 13.3, CMake 3.28.3, SQLite 3.45.1, Docker 29.1.3, OVS 3.3.9, Boost 1.92.0. Missing development tools were extracted into a private temporary build-tools directory; no system package upgrade was required.
- UI: Node 26.8.1, Chromium 153, Playwright 1.63.0. The production build emits the upstream React Flow `use client` directive warning but succeeds.
- App-a image: `sha256:820e474177962b99c1922371a0535a4dd681c69f3278382caa58526a128eb764`.
- App-b image: `sha256:20cad7bf81b8be95315fc42258ac7d602356b4757d0c70df3ac8d6bc1366e9ce`.

Images were built without build-time network access from an existing local Ubuntu base. They and temporary SQLite evidence directories were retained for diagnosis. The runtime log identifies the final evidence directory. Generated fixtures use actual image identities but retain synthetic M0 package metadata; these tests do not establish publisher authentication or package provenance.

After testing, no Graphlab-labeled containers or networks and no OVS bridges remained. Temporary browser agent/API processes, forwarding and credentials were removed. Shared OVS infrastructure and retained test images were not globally flushed.

## Qualification limits

Fine-grained interruption inside resource operations, cleanup failures, replaced namespaces, exhaustive live graph/scale coverage, GCC 14 and Linux x86-64 remain unverified. Generic topology validation/planning continues to have M0 regression coverage; the live executor evidence is the Docker ring described above. No M3 capture barrier or traffic lease, QEMU backend, continuous runtime monitor, or production service installation is claimed. See the operating guide for the bounded journal and development-profile restrictions.
