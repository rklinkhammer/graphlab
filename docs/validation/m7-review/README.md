# M7 independent verification — 2026-09-23

Verification of the working-tree M7 implementation above `e71d3f3` against the optional-backend exit criterion in `docs/option-d-console-plan.md`. Production implementation files were not changed. This directory is separate from the sealed implementation evidence in `docs/validation/m7/` and `qualification/artifacts/linux-arm64-m7/`.

## Scope and reproducibility

The dedicated ARM64 Linux VM supplies Docker, OVS, systemd, tun, and KVM. The preserved PPC64LE/TCG and ARM64/KVM inputs and pinned M7 runner image are reused. The review compares implementation/test sources with the sealed source archive, checks all 75 existing artifact checksums, rebuilds native targets, runs portable and sanitizer tests, repeats Linux backend/crash fixtures, and exercises controller-dispatched directional faults. This is qualification of the documented runtime and small-packet fixtures; it does not extend the M6 sustained-capacity envelope or qualify other host architectures.

`make-rpc-probe.mjs` generates `build/m7-review-rpc.cpp` from the existing M7 fixture without changing it. Run the generator from the repository root with Node, transfer the generated C++ file to the Linux source tree, then run `build-rpc-probe.sh` on that host. The probe uses `Engine::dispatch` for fault admission, idempotency keys, revision checks, execution and durable removal; it is not a new HTTP/browser test. Its two ARM64 host-QEMU cases cover reversed guest/Docker endpoints, foreign-port refusal, four guest-pair direction cycles and automatic fault expiry. The final cycle requires packet delivery to resume after automatic removal.

The first probe used a ten-second fault lifetime; serial replay and counter collection crossed that deadline. The journal records `fault-active` at 13:36:05 UTC and `fault-expired` at 13:36:15 UTC. That loss assertion therefore sampled after correct expiry and failed. Its log and state are retained. The final probe gives ordinary loss measurements 60 seconds and the explicit expiry case 25 seconds, with bounded expiry polling. Acceptance still requires directional loss, unaffected opposite-direction delivery, and restored delivery. The first case's resources and sentinel were recovered by their exact identities.

`linux-checks.sh` rebuilds with crash checkpoints, runs nine SIGKILL boundaries, restores checkpoints OFF, then runs privileged transport, shared-backend fault regression and the namespace-OVS rejection probe. `audit.sh` is appropriate only for this dedicated VM's otherwise empty lab runtime; it checks rather than deleting resources.

## Results

**PASS for M7's documented scope; no production-code defect identified in this review.**

| Check | Result |
| --- | --- |
| Existing sealed M7 artifacts | 75/75 checksum matches |
| Current implementation and test sources | Match sealed source archive, excluding local guest artifact directories |
| macOS build and CTest | 18/18 passed |
| macOS ASan/UBSan | 17/17 passed; installed-package consumer excluded |
| Linux rebuild and CTest | 17 passed; root-only transport skipped there and passed separately |
| Linux normal backends | 8/8 combinations passed: host/container × PPC/TCG or ARM/KVM × guest–Docker or guest–guest |
| Controller-dispatched fault/ownership probe | 2/2 cases; six fault applications, five explicit removals and one automatic expiry; four guest-pair direction cycles; foreign-port refusal and scoped recovery passed |
| Linux injected crashes | 9/9 passed; repeated recovery and unrelated sentinel preservation |
| Shared-backend M5 regression | Passed, including expiry during restart |
| Experimental namespace OVS | Rejected as designed; probe namespaces cleaned up |
| Final VM cleanup | Passed: no owned processes, units, namespaces, links, networks, containers or OVS/QoS resources |
| Normal build restored | GRAPHLAB_TEST_CHECKPOINTS=OFF |
| Existing M6 completion manifest | Integrity valid, qualified=true, unchanged |

The existing M2/M3/M4 full Linux regression logs were integrity-checked in the sealed M7 bundle; those full suites were not rerun in this review. Portable regression tests, privileged transport, and the shared M5 fault suite were rerun. Controller restart with a live direct-guest fault is not separately exercised by the new probe; direct fault expiry/removal and shared-backend restart behavior are demonstrated separately. Network/kernel isolation is not claimed for the QEMU container wrapper, and namespace-OVS remains unavailable.

Small logs are retained alongside this report. New private raw case archives, probe source/scripts, build logs, and the first failed probe's journal live in the ignored `qualification/artifacts/linux-arm64-m7-review/` directory. The separate `SHA256SUMS` inventories this new bundle. Raw archives omit duplicated immutable artifact directories and credentials; reproduce with the original sealed M7 source/image/guest-input bundle. Neither that bundle nor M6 acceptance statuses were altered.
