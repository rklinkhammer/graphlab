# M5 post-remediation verification

Date: 2026-09-22. Verified the current uncommitted workspace over M4 baseline `1f59f96`.

**Verdict: the implemented M5 scope and both remediation fixes pass the checks below. No new blocking findings were identified in this review.** This does not qualify the remaining feature and platform limits listed below.

## Review of the two findings

- **Failed expiry cleanup:** the agent durably records the fault as `recovery-required`, moves the run to `reconciling`, and explicitly requests quiescence. It records acknowledgement or failure and adds timeline events. The regression verifies this on a development run without a lease, then restarts with both removal and quiescence failing and verifies explicit failure state before successful manual recovery. Admission rejects resume and new fault application while the run is reconciling. A quiescence failure is reported, not claimed to have stopped traffic.
- **Browser/server clock mismatch:** relative history windows now derive both endpoints from one server timestamp. The regression verifies hour/day/week windows and invalid/mixed ranges. The live browser runs one minute behind the agent, loads telemetry, applies a fault, injects a history-only HTTP failure, continues displaying fault expiry and timeline updates, and recovers history without a lingering polling error or stale display.

## Fresh evidence

| Check | Result |
|---|---|
| macOS C++23 build and CTest | 13/13 passed — [log](m5-reverification-macos.txt) |
| ASan/UBSan | 12/12 passed; installed-package consumer excluded — [log](m5-reverification-sanitizers.txt) |
| Linux ARM64 current source snapshot | 12 passed; one privileged transport test skipped under the unprivileged runner — [log](m5-reverification-linux.txt) |
| Root Linux M5 packet test | Passed: one canonical counter source, asymmetric loss, expiry restoring delivery, expired-fault restart and cleanup — same Linux log |
| Browser production build and existing regressions | Build and 4/4 tests passed — [log](m5-reverification-browser.txt) |
| Live Linux browser regression | Clock skew, history outage, expiry/timeline continuity, history recovery and destroy passed — [log](m5-reverification-live.txt) |

The independent installed-package test is included in the macOS/Linux CTest results. Current remediation tests, including the final acknowledgement-wait refinement, ran on both platforms and under sanitizers. The earlier full M3/M4 root suites were not repeated; their evidence remains in the original [M5 implementation verification](m5-verification.md).

## Remaining limits

Guest→switch TAP faults still require unimplemented IFB support and are explicitly rejected. Reduced-observation profiles and separate capture-overhead accounting remain unavailable. Expiry shares the executor and is not a hard-deadline watchdog; a failed hold still requires operator recovery. History uses a global 100,000-row ceiling. Linux x86-64/GCC 14, host reboot, probabilistic-loss/delay accuracy, long-duration retention and scale remain unqualified. See [operational limits](../m5-telemetry-faults.md).

Verification added evidence/documentation only; implementation code was not modified. Preserved guest templates were unchanged. No commit was made.

Final [cleanup audit](m5-reverification-cleanup.txt) found no active Graphlab workers, owned running containers or OVS bridges. Dedicated browser services, temporary credentials and SSH forwarding were removed. The fresh [browser screenshot](m5-reverification-console.png) is saved separately from the original remediation evidence.
