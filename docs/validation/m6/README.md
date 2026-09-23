# M6 qualification evidence

This report describes the initial qualification run. See [M6 remediation](remediation.md) for subsequent fixes, additional tests and updated gate dispositions.

The M6 qualification tools are implemented and exercised. **Full M6 release qualification is incomplete: 8 of 19 required gates pass; 11 remain partial.** See the reviewed [acceptance matrix](../../../qualification/matrix.json). An incomplete required gate causes `lab-qualify verify` to exit 1 even when every artifact hash matches.

## Selected runtime and checks

Measurements ran on the ARM64 Linux lab VM: Ubuntu 24.04.4, kernel 6.8.0-134, 8 vCPUs, approximately 16 GiB RAM, Docker 29.1.3, OVS 3.3.9, QEMU 8.2.2, GCC 13 and libpcap 1.10.4. The [runtime lock](runtime-lock.json) retains exact inventory, workload image identities and binary hashes, including the post-reboot rebuild. This evidence applies to this runtime, not every ISA or deployment.

| Check | Result |
|---|---|
| Final macOS CTest | 14/14 passed |
| Address/undefined-behavior sanitizer suite | 13/13 passed; installed-package consumer excluded from this configuration |
| Final Linux CTest | 13 passed, privileged transport skipped unprivileged; separate root transport passed |
| Fresh Linux M2–M5 integration fixtures | Passed: lifecycle/crash sentinels, capture, both guest templates, Docker console, telemetry/faults |
| Browser build and graph regression | Build and four tests passed |
| Actual host reboot during capture and recorded console | Passed after the recovery fix below; five prior capture streams retained readable segments |
| Same-boot Docker console regression after fix | Passed, including missing recorder, orphan exec cleanup and parent-workload preservation |
| Evidence integrity verification | Required result is intact but incomplete; see result.json |

T11 explicitly includes historical M5 live-browser evidence for clock skew and independent refresh recovery on unchanged UI source. That live test was not rerun during M6. No earlier report is silently presented as a fresh test.

## Measured overhead and limits

The [direct-edge table](capacity-direct.md) and [three-switch triangle table](capacity-triangle.md) compare minimal observations/no capture, full telemetry/no capture, and full telemetry/all-edge capture. Each graph has 18 samples: two ten-second samples for each of three rates and three profiles. Requests contain numbered 256-byte UDP payloads. Direct capture uses a 256 MiB budget; the completed triangle run uses 1,024 MiB.

At 1,000 requests/s, direct-edge telemetry adds 0.25 host CPU percentage points versus baseline; capture plus telemetry adds 1.47 points and writes approximately 617 KiB/s. The triangle adds 0.99 points for telemetry and 2.29 points for capture plus telemetry, writing approximately 2,485 KiB/s. Neither graph reported capture drops at 250 or 1,000 requests/s in these samples.

All completed samples met the declared **delivery** envelope: ready state, at most 1% request loss and at least 95% target rate. Both graphs reported zero delivered request loss, including 4,000 requests/s. However, capture counters at 4,000 requests/s increased by as much as 16,566 in a direct sample and 63,969 summed across independent triangle workers in a sample. These are recording drops, not delivered loss; that rate is not qualified for complete recording.

CPU, available memory and paging I/O cover the whole VM, while RSS covers the controller. RTT is the mean of two sample p95s, not a pooled percentile. Lower measured RTT with observation enabled is not evidence of a speedup: short samples, fixed profile order and background activity confound that comparison. These data establish a short sampled envelope only; long-duration sustainable capacity remains T18 work.

## Failures retained and recovery fix

The first runner attempt placed capacity state beneath a user-private directory inaccessible to restricted capture workers. The runner now allocates a short root-owned parent, and direct measurements completed there. The initial triangle exhausted a hot worker's share of its 256 MiB budget and correctly quiesced; its partial report is retained alongside the 1,024 MiB follow-up. Neither failed attempt was converted into a pass or removed from the evidence.

The actual VM reboot removed temporary toolchain files, requiring a rebuild from the retained source. It also exposed a real recovery failure: cleanup queried a Docker exec ID from the prior boot and Docker returned 404. `cleanup_docker` now compares the recorded and current host boot IDs before querying the exec. A different boot proves that process cannot survive, so cleanup skips the retired process; absent boot identity still fails closed and same-boot ownership checks remain unchanged. Recovery then passed, and the existing live console suite confirmed same-boot cleanup still works.

Post-recovery inventory shows no containers, OVS bridges, active Graphlab units or live QEMU/capture/terminal workers. Retained reboot state intentionally includes journal/recording files and inactive socket directory entries; the archive omits socket objects. This observation does not substitute for T17's exhaustive partial-start cleanup matrix.

## Remaining required work

| Gate | Evidence still required |
|---|---|
| T03 | Tagged-trunk packet assertions and measured RSTP failover against a declared target |
| T04 | First numbered released packet captured on every required graph shape |
| T05 | Offload variations and dual-endpoint asymmetric delay/loss matrix |
| T07 | Fresh independent API and agent restarts during simultaneous recording |
| T09 | Concurrent real CLI/UI fault submissions |
| T10 | Remaining QEMU, terminal and fault mutation crash boundaries |
| T12 | Slow-viewer, retention eviction and recording-gap matrix |
| T14 | Hostile terminal escape and parser fuzz corpus |
| T17 | Every backend partial-start cleanup boundary |
| T18 | Sustained capacity with acceptable recording loss and declared operating limits |
| T19 | Independently released compatible protocol-minor interoperability |

## Retention and reproduction

Run instructions are in [qualification/README.md](../../../qualification/README.md). Large artifacts are retained in ignored `qualification/artifacts/linux-arm64-m6/`: independent app-a/app-b Docker image archive, installed common C++ package, independently built node sources/binaries, final source snapshot and reboot journal/recordings. Original PPC/TCG and ARM64/KVM guest templates and local artifact directories remain preserved.

This directory contains the reviewed manifest, result, runtime lock, raw measurements and logs. Verify against the **complete** ignored artifact directory, not this documentation copy: the manifest binds the large archives too. Preserve that directory when transferring the workspace. The manifest verifies bytes and explicit reviewed coverage, not the truth of arbitrary supplied log claims.
