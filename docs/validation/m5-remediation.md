# M5 remediation

The two findings in the [M5 review](m5-review.md) are corrected. The original review source and reproduction output remain historical evidence; regression tests assert the corrected behavior.

## Changes

Failed expiry or restart cleanup now first persists `recovery-required` fault state, `reconciling` run state and a quiescence request. It then asks the backend to quiesce that exact owned run, including development runs without leases. Acknowledgement or failure is saved and added to the timeline. Failed cleanup remains available for explicit remove/recover or restart reconciliation; traffic is not automatically resumed. Failure to quiesce is reported rather than represented as a successful hold.

History queries accept a bounded `windowSeconds` relative to one server timestamp. The browser uses that field instead of combining a client-derived start with a server-derived end. Explicit absolute ranges remain supported, but cannot be mixed with a relative window. Polling updates history, faults and timeline independently. A history error no longer prevents fault/expiry/timeline updates, and successful recovery clears its polling error. Browser freshness uses `performance.now()` so wall-clock adjustment cannot freeze live rates.

## Regression coverage

`tests/telemetry/remediation.cpp` exercises one-hour/day/week relative windows, invalid/mixed ranges, failed expiry removal on a development run, acknowledged quiescence, explicit recovery state and timeline, failed removal plus failed quiescence during restart, and successful manual recovery afterward. It is registered as `m5_remediation` in ordinary CTest.

The dedicated live browser regression in `console/web/tests/m5-remediation-live.spec.ts` sets the browser clock one minute behind, uses actual Linux telemetry and fault jobs, injects a history-only HTTP failure, checks that fault expiry and timeline continue updating, and then verifies history recovery clears the error and stale display. It requires the same dedicated Linux services and temporary credential as the original optional M5 browser test.

The original M5 placement and scale limits remain: guest→switch IFB support is unavailable; expiry is not an independent hard-deadline watchdog; retention has a shared capacity ceiling. See [operations](../m5-telemetry-faults.md).

## Validation on 2026-09-22

- macOS CTest: **13/13 passed**, including the new regression ([log](m5-remediation-macos.txt)).
- ASan/UBSan: **12/12 passed**, installed-package consumer excluded ([log](m5-remediation-sanitizers.txt)).
- Linux ARM64 CTest: **12 passed, one privileged transport test skipped** under the unprivileged runner; root M5 directional packet/expiry/restart fixture passed ([log](m5-remediation-linux.txt)).
- Browser production build and four existing regressions passed ([log](m5-remediation-browser.txt)).
- New live Linux browser regression passed with clock skew, injected history failure and recovery ([log](m5-remediation-live.txt), [screenshot](m5-remediation-console.png)).
- Dedicated browser services, temporary credentials and SSH forwarding were removed. No active Graphlab workers, owned running containers or OVS bridges remained ([cleanup](m5-remediation-cleanup.txt)). Preserved guest templates were unchanged.

The final portable test polling condition waits for quiescence acknowledgement rather than merely the prior durable `reconciling` state, avoiding a test race. macOS CTest was rerun after that test-only refinement; Linux/sanitizer results above exercised identical implementation code immediately beforehand. Earlier M3/M4 full regression evidence was not rerun for these targeted fixes. No commit was made.
