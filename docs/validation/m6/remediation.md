# M6 remediation

This remediation fixes both reporting defects found in [verification](verification.md), improves capture scheduling, and adds runtime and hostile-input qualification fixtures. The initial evidence bundle remains unchanged. Full M6 qualification still requires the remaining partial gates; the new tests do not automatically mark unrelated scenarios passed.

**Current disposition: 11 gates passed, eight partial. M6 remains incomplete.** T03, T05 and T14 now have the additional evidence identified by the review. T04, T07, T09, T10, T12, T17, T18 and T19 remain partial; no gate was waived or relabeled to obtain a passing release result.

## Validation

| Check | Result |
|---|---|
| macOS build and CTest | 17/17 passed |
| Sanitizer build and CTest | 16/16 passed; installed-package consumer excluded |
| Linux build and CTest | 16 passed, root-only transport skipped unprivileged |
| Full privileged M3 capture suite | Passed after the final capture-worker change; includes privileged transport |
| Tagged trunk/RSTP fixture | Passed against the 15,000 ms target; measured intervals retained in fixture logs |
| Numbered endpoint/offload fixture | All four combinations passed; forward-only delay/loss confirmed |
| PPC/TCG and ARM64/KVM guest suite | Both passed, including capture/watchdog and recovery tests |
| Browser hostile terminal corpus | Passed: seven explicit and 256 seeded sequences |
| Manifest | Intact evidence must still return exit 1 while required gates are partial |

Fresh tests support the final runtime changes. The actual VM reboot and exhaustive M2 crash suite were not repeated; their retained evidence is historical. The fixed-root triangle capacity result and raw measurements are retained with the remediation manifest.

## Code changes

- The Linux runner preserves each fixture's status, completes evidence collection, and returns nonzero after any failed or skipped required invocation. Its isolated regression tests exercise success, failure and skip codes without invoking host operations.
- Capacity summaries preserve unknown latency and capture counters. Missing workers or a missing observation interval mark capture observations incomplete; RTT deltas require known measurements on both sides. Delivery and recording outcomes occupy separate columns.
- Capture workers poll both their control socket and the libpcap descriptor. Previously, packet-ready workers could wait ten milliseconds for control activity before draining again. The requested kernel capture buffer increases from 4 to 16 MiB, within the existing 64 MiB per-worker memory limit. This increases capture memory requirements; controller RSS is not aggregate worker/kernel memory.
- `m6_network` independently reads and retains a VLAN 100 tagged frame on a real trunk. It cuts an owned forwarding link in a redundant triangle and checks delivery restoration against a **predeclared 15-second bound**, restoring the link and performing scoped cleanup afterward. The reported interval includes command/probe overhead; it is an observed upper bound, not a precise outage duration.
- `m6_direction` observes numbered datagrams separately at both Docker endpoints under forward-only 80 ms delay or 100% loss, with GRO/GSO enabled and disabled and verified through kernel readback. Capture and qdisc counters are retained separately from endpoint delivery observations.
- The terminal corpus checks 409,600 bytes of exact delayed-viewer replay across bounded pages and independent cursors, plus 512 seeded malformed recordings. The browser exercises seven explicit hostile strings and 256 seeded CSI/OSC/DCS/APC byte sequences, then checks continued rendering, no HTML execution and no attacker-directed requests. The RPC suite adds malformed requests and verifies bounded errors that do not echo supplied secrets.

## Evidence handling

The new bundle is `qualification/artifacts/linux-arm64-m6-remediation/`. It retains prior supporting artifacts under their existing names, the original source and capacity results under `initial-*`, and the new source, logs and measurements separately. Historical VM reboot, independent workload builds and guest-template evidence are explicitly reused where the corresponding functionality has not changed; those events are not represented as newly performed.

An interim capture measurement overlapped a Linux build and used the polling fix with the original 4 MiB buffer. It still reported drops and is retained as `interim-capacity.json`, not used as the clean final overhead comparison. Follow-up direct and triangle measurements use the final worker, run serially with no concurrent build, and retain the original ten-second/two-repeat sampling design. They remain bounded measurements rather than a sustained-capacity certification.

The original triangle fixture used equal bridge priorities, allowing random bridge identities to choose different forwarding paths between profiles. The final benchmark assigns distinct priorities with `s1` as root. The earlier equal-priority result remains in `triangle-equal-priority.json`; it is not the controlled final triangle overhead comparison. Direct-edge measurements are unaffected by this fixture correction.

Final direct and fixed-root triangle reports contain **zero observed capture drops and zero delivered request loss at every sampled rate**, including 4,000 requests/s. At that rate, the direct capture profile uses 4.44% host CPU and writes approximately 2,469 KiB/s; the triangle uses 9.89% and approximately 7,436 KiB/s. These are whole-VM CPU measurements, and each rate/profile has only two ten-second samples. The packet-readiness change trades more prompt work and buffer memory for fewer observed drops; it is not a guarantee of complete recording under arbitrary load. T18 remains partial.

## Work still required

The reporting defects are fixed, but the review's milestone-completion finding remains open. Required follow-up covers first released packets on every topology shape (T04), combined API/agent recording restarts (T07), concurrent real CLI/UI faults (T09), remaining mutation crash windows (T10), live slow-viewer/retention cases (T12), all partial-start cleanup boundaries (T17), sustained capacity and scaling (T18), and compatible independently released protocol-minor artifacts (T19). The retained independent builds establish current-package reuse; they cannot substitute for releases that have not been supplied or established.

The current acceptance dispositions are in [qualification/matrix.json](../../../qualification/matrix.json). The new bundle's manifest/result and reports are copied under [remediation/](remediation/); verification requires the complete artifact directory, including its archives. The original manifest continues to describe the original run only.
