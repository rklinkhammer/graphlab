# M5 implementation verification

Latest check: [post-remediation verification](m5-reverification.md) passes the implemented scope and both fixes; remaining limits are explicit.

The findings in the independent [M5 review](m5-review.md) are addressed by [M5 remediation](m5-remediation.md), with new failure-path and live browser regression evidence. The original implementation evidence below is retained.

Date: 2026-09-22. Base: `1f59f96` (M4 baseline), with the M5 workspace changes. This report distinguishes implemented/checked behavior from pending qualification. See [operations](../m5-telemetry-faults.md) for API contracts, retention and placement limits.

## Acceptance evidence

| Check | Result and evidence |
|---|---|
| macOS ARM64 C++23 build and CTest | 12/12 passed; [log](m5-macos.txt) |
| ASan/UBSan | 11/11 passed, excluding installed-package consumer; [log](m5-sanitizers.txt) |
| Independent installed package | Both independent consumers link and invoke `LabSupport::telemetry`; included in CTest |
| Linux ARM64 build/CTest | 11 passed, privileged `m3_transport` skipped by unprivileged runner; [log](m5-linux.txt) |
| Real directional traffic/netem | Root `m5_linux` passed on Docker direct edge; same [Linux log](m5-linux.txt) |
| Browser regression | Four existing Playwright tests passed; [log](m5-browser.txt) |
| Live Linux browser | Fault preview/apply/expiry, directional history/chart, timeline and destroy passed; [log](m5-browser-live.txt), [screenshot](m5-console.png) |
| Capture regression | Full root M3 suite passed; [log](m5-capture-regression.txt) |
| Guest/console regression | PowerPC64LE/TCG, ARM64/KVM and Docker recorded-console checks; [log](m5-guest-regression.txt) |

The portable M5 suite checks unknown first samples, directional deltas, uint64 precision beyond JavaScript's safe-integer range, counter reset/mapping/outage gaps, aggregate gap counts, retention boundaries, bounded queries, typed fault bounds, idempotency, stale revisions, overlapping directions, expired-fault restart and compensation of cancellation during an executing apply.

The Linux fixture sends 40 one-way UDP datagrams into an owned direct Docker edge and verifies delivery plus a single canonical TX counter increment (allowing ARP, never doubling endpoints). A→B 100% loss prevents delivery while all 40 B→A packets arrive. Expiry restores delivery; an agent restart removes another expired fault before recovery cleanup. This proves directional loss placement; it is not a latency-distribution or probabilistic-loss accuracy benchmark.

The live browser test uses a five-edge Docker/OVS topology and a three-second delay fault. The earlier M3/M4 regression suites exercise captures, pause/resume, outage leases, terminal recording/replay and cleanup with M5 collection enabled. They are regressions, not measurements of every TAP/OVS telemetry direction.

## Reproduction

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
npm ci --prefix console/web
npm run build --prefix console/web
npm test --prefix console/web -- tests/console.spec.ts
```

Privileged tests require a dedicated Linux Docker/OVS/systemd host with the M2 C++ fixture image built and the existing capture/terminal trusted-worker setup. Do not run concurrent root fixtures or an agent holding `/run/graphlab-executor.lock`.

```sh
sudo build/dev/m5_linux "$PWD" "$(sudo docker image inspect graphlab-m3/app-a:verification --format '{{.Id}}')"
```

The optional `GRAPHLAB_M5_LIVE=1` Playwright fixture uses a dedicated authenticated API on port 18089, an M2 development topology, and a temporary credential file `build/m5-live-password`. It creates and destroys a real run. It is skipped by default. See the M3/M4 verification documents for their explicit root regression commands.

## Boundaries

- Runtime evidence is Ubuntu 24.04 ARM64, GCC 13, kernel 6.8, Docker 29.1.3, OVS 3.3.9 and systemd 255. macOS checks portable code/UI only. Linux x86-64 and the reference GCC 14 preset remain unqualified.
- Guest→switch TAP netem needs IFB and is explicitly unavailable. Guest-direction fault acceptance, host-reboot fault recovery, foreign-qdisc collision races, exact delay distributions and scale/long-duration retention remain unqualified.
- Fault expiry shares the executor. It can be late during long jobs or an agent outage; restart/resume reconciliation is implemented. There is no independent hard expiry watchdog.
- History retention has a global 100,000-row capacity, so large/retained runs can shorten the stated time windows. Unit tests cover aggregation and query bounds, not seven days of wall-clock collection or disk-capacity exhaustion.
- Browser polling exposes stale observations and chart gaps. Visual reset/gap scenarios are covered by C++ rate tests and chart logic, not dedicated automated browser fault-injection tests.
- Reduced-observation profiles, per-capture overhead accounting, and richer graph overlays are not implemented in M5. Current health, mapping, qdisc and RSTP details are available in the telemetry panel and API.

No Python was added. Test credentials and SSH forwarding are removed after live verification; guest templates and their preserved artifacts remain unchanged. Changes are not committed by this task.

Final [cleanup audit](m5-cleanup.txt) found no active Graphlab VM/capture/terminal workers, owned running containers, OVS bridges or dedicated browser agent/API processes.
