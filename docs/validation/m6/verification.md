# M6 verification

This is the pre-remediation review. See [subsequent fixes and evidence](remediation.md) for the current remediation results.

Result: **not qualified**. The retained manifest is intact and correctly returns exit 1 (`qualified: false`). It records eight passed gates and eleven partial gates. Verification also reproduced two defects in the qualification tooling. No production code or historical evidence was changed during this review.

## Findings

1. **P1 — M6 exit criteria remain unmet.** The plan requires all T01–T19 gates to pass on the selected runtime. T03, T04, T05, T07, T09, T10, T12, T14, T17, T18 and T19 remain partial. The matrix accurately discloses those gaps; passing existing unit/integration suites does not close them. In particular, bounded ten-second capacity samples do not establish sustained capacity, and the retained 4,000 requests/s samples contain capture drops despite zero delivered request loss. See [the disposition and remaining work](README.md).

2. **P2 — The Linux runner returns success after fixture failures.** In `qualification/run-linux.sh:18–24`, `run` logs a failing command's status but returns the final successful `echo`; the script's final command is also an `echo` at line 64. With runtime-inventory commands succeeding, a failed integration suite therefore leaves the runner exit code at zero. A PATH-isolated reproduction made all eight fixture commands return 42; `exits.txt` correctly recorded every failure, but the runner returned 0. This can mislead shell/CI callers even though a human can inspect the log and the separate manifest verifier remains fail-closed. Accumulate fixture failures, finish collecting evidence, then return nonzero when any required invocation fails. Reproduction: `build/m6-review-runner/reproduction.txt`.

3. **P2 — Capacity summaries turn unavailable measurements into zeroes.** `cpp/qualification/main.cpp:67` substitutes zero for a null p95 RTT; lines 75–84 skip null capture-drop values while keeping the total at zero. In a copy of the retained capacity JSON, replacing capture drop observations with null yielded “0” drops and the unchanged passing delivery envelope. A separate no-reply sample with null RTT yielded “0.00 µs” and “−100.00%” RTT change. These imply measurements that do not exist. Report unknown values explicitly, propagate incomplete capture coverage, and suppress RTT deltas when either side lacks latency observations. Add summarizer tests for these cases. The original retained reports have the relevant observations and their tables reproduce exactly; the reproduction did not alter them. Files: `build/m6-review-unknown-drops.{json,md}` and `build/m6-review-no-replies.{json,md}`.

## Fresh checks

| Check | Result |
|---|---|
| Development build and macOS CTest | Passed, 14/14 |
| Sanitizer build and CTest | Passed, 13/13; installed-package consumer excluded |
| Linux build and CTest | Passed, 13 tests; root-only transport skipped in unprivileged CTest |
| Separate privileged Linux transport | Exit 0 |
| Live Linux Docker console suite | Passed, including orphan exec cleanup and parent workload preservation |
| Live Linux telemetry/fault suite | Passed, including asymmetric packet loss, expiry and restart removal |
| Web TypeScript/production build | Passed; existing bundle-size/directive warnings |
| Retained manifest | All 39 artifact hashes verified; exit 1 for incomplete gates, as intended |
| Archived source | All 116 archived files match current workspace bytes |
| Capacity tables | Both regenerate byte-for-byte from retained raw JSON |
| Linux source comparison | All six reviewed M6/runtime source files match local bytes |
| Post-test cleanup | No containers, OVS bridges or active Graphlab units reported |
| Whitespace check | `git diff --check` passed |

Fresh logs are `build/m6-review-ctest.txt`, `build/m6-review-sanitizers.txt`, `build/m6-review-linux.txt`, `build/m6-review-web.txt`, and `build/m6-review-integrity.json`. The review used the existing ARM64 Linux VM and preserved guest templates.

The complete M2 crash matrix, full M3 capture suite, PPC/TCG and ARM64/KVM guest suite, actual reboot, capacity sweep and browser interaction tests were **not rerun** in this verification. Their retained evidence was integrity-checked; this report does not relabel it as fresh execution. A fresh actual reboot is unnecessary to reproduce either tooling defect. The original artifact manifest and its matrix remain unchanged for auditability.
