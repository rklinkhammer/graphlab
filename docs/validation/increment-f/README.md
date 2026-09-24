# Increment F: console qualification — 24 September 2026

**Completed for the implemented A–E scope on the dedicated Linux ARM64 host.** Fresh checks cover the current controller/console, qualified opt-in Docker fixtures, PPC64LE/TCG and ARM64/KVM guests, retained recordings/logs/captures, authorization and failure recovery. This is not universal GraphX parity, a new multi-host/platform certification or an unbounded capacity guarantee.

One production defect was found and fixed: the message panel's long identifiers caused horizontal overflow at 390px. Scoped `overflow-wrap: anywhere` preserves identifiers while keeping the narrow layout within the viewport. Added regression coverage for this view, keyboard button activation, expired-session errors, late responses after node selection and correlation errors surviving successful history polls. Other runtime production code is unchanged.

## Acceptance and evidence

| Area | Fresh result |
|---|---|
| Build and portable regression | [Build](native-build.txt), **26/26 passed** [CTest](native.txt); final formatted [bounds test](bounds.txt). Tests include contracts, peer/session checks, journal recovery, source queues, telemetry limits, packet maintenance, log quotas and message recovery. |
| Dedicated Linux build/tests | [Fresh full build](linux-build.txt). Final [CTest](linux-native-final.txt): **25 passed, one root-only test skipped**. That skipped worker-transport test was separately run as root and [passed](linux-transport.txt). Installed SDK consumers passed on Linux after fixing the qualification shell's tool PATH. |
| Production console and browser | [Build](console-build.txt) passed. [Default suite](browser.txt): **29 passed, 15 environment-gated skips**. Enabled runtime suites are separately recorded below; skips are not counted as live verification. |
| A: endpoint-owned application metrics | [Real two-stream test](edge-live.txt) passed with alpha 5 / beta 3 messages, 80 / 48 bytes and 4 / 2 reconnects, independently compared against endpoint counters. Legacy reports remained absent, and auth/limits/restart/destruction checks passed. |
| B: source controls | [Live test](source-live.txt) passed: alpha stayed 17 while beta advanced 22→33; independent PCAPNG inspection found zero alpha and 10 beta datagrams inside the pause interval. Capture processes stayed unchanged, receive/echo continued, run quiesce remained authoritative and journal state survived restart. |
| C/D: logs and capture inspectors | [Initial live test](inspectors-live.txt) matched five artifacts / 1,408 packet records. The expanded [workload-restart test](inspectors-restart-live.txt) matched five artifacts / 1,307 records, independently compared exact Docker log bytes, tail rollover and 65,536-byte truncation, retained downloads, distinct process generations, old-artifact preservation, API/agent restart and destruction. |
| E: messages/correlation | [Live fixture](messages-live.txt) passed with independent send/receive reports and packet bytes. Selected edge `b-s` gave artifact `aa0ce623dbec6bed6c4a9677-0`, packet 6, block offset 680; the run capture union gave three occurrences and ambiguity. Authorization and retained correlation after agent restart/destruction passed. |
| Persistence exhaustion | [Fresh actual ENOSPC tests](enospc.txt) on separate **8 MiB temporary tmpfs** filesystems passed for messages, process logs and application-edge telemetry. Committed state stayed intact; freeing space allowed complete retry. [Harness](enospc.sh), [new application fixture](../../../tests/qualification/application_full_linux.cpp). |
| Message bounds | [New native cases](bounds.txt) prove the 256-source admission ceiling without eviction, existing-source continuity at capacity, 24-hour expiry without replay resurrection, 4 MiB body pruning before 4,096 records (4,194,204 bytes / 4,076 records), 16-segment, 64-match, 2,000-record and 16 MiB scan limits, and no correlation of ESP bytes. [Test source](../../../tests/message_bounds.cpp). |
| Docker recorded consoles | [Fresh Linux fixture](docker-console.txt): PTY input/resize, writer fencing/release, identity-checked logs, replay through controller restart, explicit exec cleanup and scoped teardown passed. |
| Terminal privacy/bounds/late reader | [Real terminal fixture](terminal-bounds-linux.txt) passed for default input exclusion, explicit input recording, bounded replay, independently replayed late viewers, recorder quota/partial artifacts and cleanup. Portable terminal corpus tests also passed. |
| QEMU management/serial/recovery | [PPC64LE/TCG and ARM64/KVM](qemu-linux.txt) passed: capture-first startup, pinned authenticated management SSH/PTY, serial replay, traffic cessation during controller outage, independent watchdog, supervisor interruption, retained partial/closed recordings, data-interface SSH exclusion and failed-boot cleanup. |
| QEMU retained logs | [Native worker log](qemu-logs.txt) independently matched invocation-scoped journal bytes and survived destruction. [Container-runner case](qemu-runner-logs.txt) also passed separate runner byte comparison and retained access. Empty runner output is valid; this is not a claim of nonempty runner diagnostics. |
| Source identity and cleanup | [134 source/test/asset hashes](source-hashes.json) match local and isolated Linux copies. [Cleanup](cleanup.txt): all task API/agent units inactive, no running containers, OVS bridges, capture/terminal/QEMU workers or test tmpfs mounts. |

The existing default browser suite also exercises application/network association navigation, legacy topologies, disconnected/isolated graphs, parallel edges, stale/unknown evidence, safe terminal rendering and logout. Reverse/parallel/self-edge layout coverage is a layout test; it is not represented as a live network experiment. Native router tests exercise actual short-lived session expiry; the new message-panel browser case injects a 401 to verify its presentation. Live HTTP suites exercise real unauthenticated, CSRF, Origin and wrong-scope rejection.

Screenshots (the narrow message, live correlation and capture metadata views were visually reviewed): [narrow message panel and keyboard focus](mobile.png), [two application streams](application-edge-live.png), [source controls](source-control-live.png), [capture metadata](inspectors-live-capture.png), [retained logs](inspectors-live-logs.png), and [message correlation](messages-live.png). Sticky navigation may overlay the top of a scrolled element screenshot; the narrow viewport regression separately asserts that the entire document fits the viewport.

## Reproduction and environment

Base checkout: `59bcd4fed004562a71eb3fe72ff84c482529ed51`, plus the source changes hash-bound above. The current source was copied to **`/tmp/graphlab-f-20260924`**, configured and built independently. [Build script](build-linux.sh); [runtime/image/guest disk identities](environment.txt).

Discovered no active Graphlab services, containers or OVS bridges before qualification. Native Docker/QEMU fixtures ran serially with live agent suites. Fresh API fixtures used:

| Feature | Private fixture root | Units | Loopback port |
|---|---|---|---|
| A | `/var/tmp/graphlab-f-edge-20260924` | `graphlab-f-edge-agent`, `graphlab-f-edge-api` | 18095 |
| B | `/var/tmp/graphlab-f-source-20260924` | `graphlab-f-source-agent`, `graphlab-f-source-api` | 18096 |
| C/D | `/var/tmp/graphlab-f-inspectors-20260924` | `graphlab-f-inspectors-agent`, `graphlab-f-inspectors-api` | 18098 |
| E | `/var/tmp/graphlab-f-messages-20260924` | `graphlab-f-messages-agent`, `graphlab-f-messages-api` | 18099 |

[Setup](setup-fixtures.py) creates new roots from existing qualified topology/lock and private authentication files, verifying image availability without printing credentials. It refuses existing output roots. [Live runner](run-live.py) starts each root agent / UID-501 API in turn, uses the explicit environment overrides added to the live tests, propagates failures and stops its services. It requires the prepared fixtures, deployed assets and existing Lima SSH configuration. Before rerunning, inspect availability and any retained run requiring recovery; do not delete old state to bypass ownership checks.

Native runtime commands used the fresh build:

```sh
sudo /tmp/graphlab-f-20260924/build/dev/m4_linux /tmp/graphlab-f-20260924 \
  /var/tmp/graphlab-m6-guests/ppc /var/tmp/graphlab-m6-guests/arm
sudo /tmp/graphlab-f-20260924/build/dev/m4_console_linux /tmp/graphlab-f-20260924 \
  sha256:bf955aa5f7ec16950e97ca194a9884292a16cd6520955a3c69d3489a38da2f11
sudo /tmp/graphlab-f-20260924/build/dev/m6_terminal_live /tmp/graphlab-f-20260924 \
  sha256:bf955aa5f7ec16950e97ca194a9884292a16cd6520955a3c69d3489a38da2f11
```

The `m4_linux ... --fixtures` preparation mode made new private log-test roots [recorded here](qemu-log-fixtures.txt); `process_log_linux --qemu ROOT [RUNNER_SHA256]` then exercised native and container-backed worker logs. The first runner attempt used a nonexistent `latest` tag and then an invalid repository-prefixed runner identifier. The final check uses the inspected bare SHA-256 required by the contract. No validation was bypassed. Similarly, the initial Linux CTest invocation lacked Ninja on PATH; [initial output](linux-native.txt) and [follow-up](linux-followup.txt) are retained, and the complete corrected invocation passed. The ENOSPC fixture initially queried node rather than edge scope; it was corrected before its successful evidence run.

## Scope and remaining limitations

A–E are qualified here as **bounded, opt-in features**. No capture/serial ownership model or capture-first admission behavior was changed. Exact correlation still means a verified occurrence within the scanned retained scope, not delivery, route or global identity uniqueness. Message clocks remain local and unsynchronized. Payload recording in message history stays disabled.

Not newly qualified: whole-host power loss/reboot, new runtime/ISA distributions, sustained saturation across all optional collectors, actual Docker log-driver file rotation, arbitrary protocol/QEMU message reporting, TCP reassembly, encrypted correlation, or application retry/fan-out delivery accounting. The SQLite 4,096-page setting remains a physical backstop; this run proves logical body/record/source bounds and actual ENOSPC atomicity, not an independent exact-page-boundary exhaustion scenario. No pending source-command SIGKILL at every instruction boundary is claimed; source uncertainty/recovery uses native state-machine tests and fresh real controller restarts.

The previously documented no-reset lifetime source/command ledgers, discontinuous log snapshots, finalized-only packet indexing, retained-scope correlation and absent synchronized latency remain product limits, not zero-valued substitutes. F closes the scoped A–E qualification increment; full parity remains limited by these explicitly unsupported capabilities. Operational startup instructions remain [Run the console for a specific topology](../../run-console.md).
