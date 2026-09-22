# M5 verification review

Follow-up: both findings are addressed in [M5 remediation](m5-remediation.md). This review and its original reproductions are retained as historical evidence.

Date: 2026-09-22. Reviewed the uncommitted M5 workspace over `1f59f96` (M4 baseline).

**Verdict: remediation required.** Existing acceptance tests pass, but two additional probes reproduce failure cases outside that coverage. Application implementation was not changed during this verification.

## Findings

### P1 — Failed expiry cleanup does not quiesce the run

Location: `cpp/telemetry/engine.cpp`, `Engine::m5_monitor`, expiry removal catch (lines 270–275).

When fault removal throws, the journal changes the run to `reconciling` and leaves the fault in `removing`, but never calls `backend_.gate(run, "quiesce")`. The later capture/lease monitor only selects `ready` runs, so it cannot perform its normal explicit quiescence path. An explicitly supported Docker development run has no traffic lease and can continue indefinitely with the expired/uncertain fault. Subsequent M5 monitor passes only attempt expiry for `active` faults, so this `removing` state is not retried automatically.

The probe applies a one-second fault, injects a removal failure and observes `reconciling`, `faultRecoveryError=injected_remove_failure`, fault state `removing`, and zero additional quiesce calls. Manual recovery succeeds after clearing the injected failure. See [probe source](m5-review-probe.cpp) and [output](m5-review-probe.txt).

Remediation: make expiry/recovery failure explicitly quiesce the exact owned run, persist acknowledgement/failure and a timeline event, and retain a recoverable fault state. Add a regression covering a no-lease development run and removal failure; do not rely solely on capture leases eventually expiring.

### P2 — Browser history windows mix client and server clocks

Location: `console/web/src/telemetry.tsx:10`; `cpp/telemetry/store.cpp:106` and its range check at line 114.

The browser computes `fromUnixSeconds` from its own clock minus the maximum allowed window, omits `toUnixSeconds`, and the server fills the end using its clock. With a browser one second behind, the requested hour becomes 3,601 seconds and is rejected as `invalid_telemetry_range`. Even synchronized clocks can cross a second boundary in transit. The same issue affects day/week selection. Because refresh uses `Promise.all`, this also prevents otherwise successful fault/timeline results from updating the panel, which eventually becomes stale.

The probe sends precisely the last-hour request produced with a one-second lag and confirms rejection. Prefer a server-relative duration request, or construct both endpoints from one agreed clock basis. Test clock skew and a request crossing a second boundary; keep independent fault/timeline refresh usable when history fails.

## Fresh checks

| Check | Result |
|---|---|
| macOS development build and CTest | 12/12 passed — [log](m5-review-macos.txt) |
| ASan/UBSan | 11/11 passed, installed-package test excluded — [log](m5-review-sanitizers.txt) |
| Linux ARM64 current source snapshot | 11 passed, privileged `m3_transport` skipped under unprivileged CTest; root M5 fixture passed — [log](m5-review-linux.txt) |
| Browser production build and existing regression | Build passed; 4/4 tests passed — [log](m5-review-browser.txt) |
| Additional portable failure probes | Both findings reproduced, isolated fixture cleaned — [output](m5-review-probe.txt) |

The root M5 fixture again confirms single-source directional packet accounting, A→B loss without B→A loss, timed expiry restoring delivery, expired-fault restart cleanup and run teardown. The independent package test exercises the installed telemetry export. These passing cases do not cover the failures above.

The previous live M5 browser and M3/M4 root regression evidence remains in [implementation verification](m5-verification.md); those longer suites were not rerun during this review. Neither were Linux x86-64/GCC 14, host reboot, probabilistic loss accuracy or long-duration/scale retention qualified. Guest→switch IFB support, reduced-observation profiles and separate capture-overhead accounting remain known implementation gaps, not newly discovered regressions.

## Reproduce the additional probes on the verified macOS toolchain

After building `build/dev`, from the repository root:

```sh
/usr/bin/c++ -std=c++23 -g -isystem /opt/homebrew/include \
  -Iinclude -Ipackages/lab-support/include -Ibuild/dev/_deps/nlohmann_json-src/include \
  docs/validation/m5-review-probe.cpp -o build/m5-review-probe \
  build/dev/libgraphlab_runtime.a build/dev/libgraphlab_catalog.a \
  build/dev/packages/lab-support/liblab_telemetry.a \
  build/dev/packages/lab-support/liblab_contracts.a \
  build/dev/_deps/yaml-cpp-build/libyaml-cppd.a \
  -L/opt/homebrew/opt/openssl@3/lib -lcrypto -lsqlite3
build/m5-review-probe "$PWD"
```

This diagnostic deliberately succeeds when it reproduces the two bugs. It is not a passing acceptance test for corrected behavior. It reuses existing portable fixture helpers and does not create Docker or network resources.
