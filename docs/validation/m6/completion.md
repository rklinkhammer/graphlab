# M6 acceptance completion

All eight remaining gates have passing Linux evidence: **T04, T07, T09, T10, T12, T17, T18 and T19**. The [current matrix](../../../qualification/matrix.json) contains 19 passed gates. This extends the original qualification and remediation; their bundles and reports remain unchanged. M7 has not begun.

The new bundle is `qualification/artifacts/linux-arm64-m6-completion/`. It retains source, logs, journals, recordings, installed SDKs, image archives, runtime identities and a hash-bound manifest. Large archives are intentionally ignored by Git: preserve this directory when transferring the workspace. Small reports and the reviewed manifest are mirrored under [completion/](completion/). Verification requires the complete bundle, not only the mirrored manifest.

## Declared scope

[Capacity limits](../../../qualification/remaining-gates.md) and the [protocol-minor matrix](../../../qualification/protocol-minor-matrix.md) precede their respective tests. The selected runtime is the eight-vCPU ARM64 Ubuntu lab VM, with PPC/TCG and ARM64/KVM guest fixtures. This does not certify other host ISAs, arbitrary topology sizes or unlimited load. Historical reboot, M2 crash and other unaffected evidence is retained explicitly; those events are not claimed as newly repeated.

## T04, T07, T09

`m6_barrier` passed chain, star, ring, mesh, disconnected, isolated, parallel, triangle and multi-NIC. Prepared senders use verified capture-point namespaces and wait for every worker to report active before release. Independent libpcap readback finds each point's unique sequence-zero marker. Every shape also has an arm-failure case that holds senders; isolated has zero edges and is explicitly vacuous. These tests prove first-frame recording at declared capture points. Endpoint delivery has separate fixtures.

The live browser/CLI service fixture admits exactly one of two concurrent fault requests, returns the same job on same-key replay, and rejects conflicting bodies and stale revisions. Independent API/agent restarts preserve capture invocations and recorded Docker output, fence stale writers and avoid duplicate session imports. Killing a capture worker subsequently marks coverage incomplete.

The first service run exposed a terminal-close race: killing a Docker exec could close its recorder socket before systemd observed exit. Bounded retry and supervisor readback fix recovery. Both the failed attempt and successful runs are retained.

## T10, T17

**45/45 SIGKILL cases and 45/45 process/namespace-FD audits passed.** Cases cover durable intent, creation, readback and completion for both guest architectures, first/second TAP ownership, management attachment, overlays, Docker/SSH terminals, and netem apply/remove. Each performs two recoveries, denies cancellation during teardown while cleanup continues, and preserves unrelated bridge/link/container sentinels.

The fixtures and final inventory exposed and fixed four cleanup problems:

- TAP creation formerly made a persistent link before assigning its ownership alias. A new TAP now remains descriptor-owned and nonpersistent until its alias is installed; executor death before that point removes it automatically.
- Repeated Docker exec cleanup failed after its parent container had retired the exec ID. A confirmed Docker 404 is now treated as already removed; other inspection failures still fail.
- Capture/terminal stop can race systemd unit collection. Stop failure is accepted only after independently confirming absence. Verified inactive workers' stale control, attach, serial and QMP sockets are removed.
- A failed transient worker can retain a systemd failure record after `stop`. Cleanup now resets only the verified, stopped unit and requires its collection. The initial failed-unit inventory is retained, followed by explicit descriptor-scoped cleanup and fresh capture, terminal-quota and guest failure regressions.

Audits retain link, Docker, OVS and unit inventories and scan `/proc` for processes or FDs retaining recorded container namespaces. No owned runtime resources remain. Recordings, manifests and overlays are retained intentionally. Failed attempts and their scoped recoveries remain in `stage2-artifacts.tar.gz`.

Crash injection requires `-DGRAPHLAB_TEST_CHECKPOINTS=ON`; deployment builds default OFF and ignore `GRAPHLAB_CRASH_AT`. Normal binaries were rebuilt with checkpoints OFF before production regressions and capacity measurements.

## T12

The native live fixture proves one-writer exclusion, applied/retained resize, exact output offsets and independent late replay. With echo disabled, default recording excludes a generated secret and opt-in mode retains its exact input bytes. A 64 KiB test budget produces `recording_quota_exhausted`, preserves its earliest complete records without overwrite, exposes a partial artifact and preserves its digest through repeated cleanup. This verifies the declared stop-on-quota policy.

The WebSocket fixture pauses actual TCP reads while queuing 200 replay requests. Another peer receives all 4 MiB of live output. Disconnect/reconnect and late independent replay match every record. The first successful run matched 1,100 records and observed API RSS growth of 324 KiB. Final-run details are retained separately. Browser and malformed-recording corpora remain additional evidence.

## T18

**All twelve 300-second samples passed**, with successful cleanup. Each graph ran baseline, telemetry-only and all-edge capture for two consecutive samples per profile: one hour of measured traffic in total. No build or other fixture overlapped the Linux measurements.

| Graph/profile | Delivered req/s | Loss | Mean sample p95 RTT | Mean host CPU | Recording rate |
|---|---:|---:|---:|---:|---:|
| Direct, baseline | 1,000 | 0% | 0.355 ms | 0.98% | — |
| Direct, telemetry | 1,000 | 0% | 0.349 ms | 1.83% | — |
| Direct, capture | 1,000 | 0% | 0.427 ms | 3.44% | 647 KiB/s |
| Triangle, baseline | 1,000 | 0% | 0.449 ms | 1.78% | — |
| Triangle, telemetry | 1,000 | 0% | 0.421 ms | 3.15% | — |
| Triangle, capture | 1,000 | 0% | 0.497 ms | 6.43% | 1,950 KiB/s |

Every sample passed the fixed <=0.1% loss, >=99% achieved rate, <=20 ms p95 RTT, <=25% host CPU, >=2 GiB available memory and <=64 MiB within-profile RSS-growth limits. The worst observed sample p95 was 0.524 ms, CPU 7.33%, minimum available memory 14.59 GiB and maximum RSS growth 13.86 MiB. One-second health observations found continuous readiness and unchanged capture worker identities. Capture source-drop increments were zero; unavailable interface-drop counters remain unknown.

This qualifies two Docker endpoints, 256-byte numbered requests/replies at 1,000 requests/s, on the direct one-edge and fixed-root five-edge triangle graphs, for the measured duration. It does not extrapolate the earlier 4,000 req/s stress observations. The exact pre-T19 measurement source is `capacity-source.tar.gz`; raw reports, health samples, packet recordings and measured binary hashes are in `stage4-artifacts.tar.gz`. Final source additionally adds the T19 protocol checks and stopped-unit collection fix. The reports identify these source phases rather than claiming an unmeasured universal capacity limit.

## T19

Complete local SDK releases **1.0.0 and 1.1.0** independently build app-a and app-b in four node-only contexts. Exact SDK hashes and immutable image IDs enter the fixture artifact locks. Every resulting image passes the no-Python check. The frozen legacy controller and current controller each pass all four same/mixed minor pairings, bidirectional probes, quiesce and cleanup. The current controller additionally rejects a wire-major-2 fixture before releasing either node, even though its image label falsely advertises v1. Both fixtures reject an unsupported workload-contract major. Portable tests cover missing/known minors, malformed versions, optional additions and required-feature rejection.

The old SDK archive lacked `share` CMake metadata needed by clean consumers. It remains unchanged; a complete 1.0.0 SDK was rebuilt from the frozen source with pinned dependencies. New SDK archives include `include`, `lib` and `share`. The failed packaging/toolchain attempts are retained. These are local qualification releases, not published registry artifacts or a cross-compiler ABI promise.

`stage5-artifacts.tar.gz` retains SDKs, node source contexts/binaries, version hashes, the frozen controller binaries and real runtime proofs. `protocol-images.tar.gz` retains all four application/version images plus the incompatible test image. The frozen controller uses the pre-T19 runtime in `capacity-source.tar.gz` with `tests/qualification/protocol.cpp` and its CMake target added before the protocol change; its executable hashes are recorded in the freeze log. `protocol-1.0-source.tar.gz` is the immutable common/node source input for the 1.0.0 SDK rebuild.

## Reproduction and checks

Use the [qualification instructions](../../../qualification/README.md) and explicit staged fixtures. All mutating Linux tests serialize on `/run/graphlab-executor.lock`; workers must be root-owned 0755. Service fixtures accept `GRAPHLAB_SSH_CONFIG`, `GRAPHLAB_LINUX_SOURCE` and `GRAPHLAB_LIVE_ROOT` in the browser tests. Retained failures can be scoped-recovered with `m6_crash --recover EVIDENCE_ROOT`; do not globally delete Docker or network resources.

Final portable checks: macOS CTest 18/18; ASan/UBSan 17/17 with installed-package consumer excluded. Linux CTest passes with the root-only transport test skipped unprivileged, followed by a separate successful privileged transport run. The full M3 capture suite, M4 Docker console suite and PPC/TCG plus ARM64/KVM guest suites passed after the cleanup changes. The nine-shape first-frame fixture was also repeated after the protocol change. Normal browser tests pass separately from explicit live service tests; unrelated opt-in live fixtures are reported as skipped, never counted as passed.
