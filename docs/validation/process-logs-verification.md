# Increment C verification — 23 September 2026

Original verification result: remediation needed for one reproduced console defect. Server-side integrity rejection and the exercised retained-log lifecycle passed. The subsequent remediation below resolves the reproduced defect.

## Finding

**P2 — catalog polling erases download/integrity failures.** In `console/web/src/retained-logs.tsx:9`, every successful catalog refresh clears the same `error` state set by `load()` at line 21. On the newest page, a rejected checksum/download therefore loses its alert on the next three-second poll, without a successful download, dismissal, or scope change. The selected artifact is cleared, leaving no persistent explanation for the failed action. The server continues to reject corrupted bytes; this is a presentation failure, not an integrity bypass.

Reproduced by extending the existing checksum-error browser test: assert the alert, wait for `queries` to advance, then assert the alert remains. The final assertion fails because the alert no longer exists. See [failure output](process-logs-verification-error-repro.txt) and the [minimal test patch](process-logs-verification-error-repro.patch). The temporary failing test was removed from the normal suite after recording this evidence.

Remediation: separate catalog-refresh errors from selected-artifact/action errors. Clear action errors only for an explicit retry/new action, dismissal, or scope change. Add a regression test proving checksum and other download failures survive successful background catalog refreshes.

## Fresh verification

- [Native build](process-logs-verification-build.txt) and [CTest](process-logs-verification-native.txt): 24/24 passed.
- [Console production build](process-logs-verification-console.txt) passed, with existing bundle-size/module-directive warnings.
- [Existing focused retained-log browser test](process-logs-verification-browser.txt) passed; it checks the checksum alert immediately, which misses the finding above.
- [Full existing browser suite](process-logs-verification-browser-all.txt): 22 passed, 13 environment-gated tests skipped. Dedicated Linux execution is recorded separately below.
- [Dedicated Linux Docker/API/browser run](process-logs-verification-live.txt): passed. Independent Docker byte comparison, authentication/CSRF/Origin rejection, node isolation, tail-window rollover, exact 65,536-byte truncation, browser byte download, agent restart and retained API/console access after destruction.
- [Dedicated Linux store and actual ENOSPC](process-logs-verification-linux.txt): passed. A dedicated 8 MiB tmpfs filled to exhaustion rejects publication while preserving committed artifacts, then succeeds after freeing space.
- [Fresh QEMU worker/container-runner qualification](process-logs-verification-qemu.txt): passed. Nonempty worker journal matched independent invocation-scoped journal output. Runner artifact matched Docker output, which was empty. Owned run destruction preserved retained downloads.
- [Source hashes](process-logs-verification-source.txt) match for the local and Linux store, runtime engine/backend, worker and qualification sources.
- [Cleanup](process-logs-verification-cleanup.txt): temporary API/agent inactive; no running containers, capture/terminal workers or OVS bridges remained.

## Coverage limits

The bounded snapshot design is explicit and appropriate to this increment's extension of runtime tails. It cannot recover output discarded before attachment or between polls. The suite exercises tail-window rollover, not actual Docker log-driver file rotation; source-generation transitions use injected native snapshots rather than a real workload restart. No dedicated slow-consumer/stalled-runtime isolation test or power-loss test was added by this verification. Runner comparison covers an empty stream, not nonempty stderr. These limits should remain visible rather than treating the original acceptance list as completely qualified.

Fix the console finding and add its regression test before treating C as fully verified. The remaining runtime qualification limits should either receive targeted tests or remain explicit scope limitations.


## Remediation — 23 September 2026

Separated `queryError` and `actionError` in the retained-log viewer. Successful catalog refreshes clear only query errors. New artifact actions and explicit scope/navigation changes clear action errors; artifact success does not erase an unrelated catalog failure. Existing selection-generation checks still reject late artifact responses.

Two browser regressions cover checksum rejection and network failure. Each waits for a later successful poll to render before checking the artifact alert, checks simultaneous catalog/action failures, retries successfully without clearing the catalog error, verifies catalog recovery, and verifies source changes clear the prior artifact error and content. [Focused results](process-logs-remediation-focused.txt): 2/2 passed. [Production console build](process-logs-remediation-build.txt) passed. [Full browser results](process-logs-remediation-browser.txt): 24 passed, 13 environment-gated tests skipped.

The reproduced P2 defect is resolved. This remediation changes console code/tests only. Native and dedicated Linux backend checks were not rerun; their preceding verification evidence remains applicable to the unchanged backend. Runtime qualification limits listed above remain explicit and are not closed by this UI fix.
