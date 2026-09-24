# Increment C reverification — 23 September 2026

Disposition: the previously reproduced P2 console defect is resolved. No new actionable finding arose from the reviewed change. Verification remains limited to the documented bounded, discontinuous snapshot contract.

The viewer keeps catalog and artifact-action errors separate. Successful background polling cannot clear checksum/network download failures. Successful artifact reads cannot clear catalog errors. New actions and explicit selection-scope changes clear the corresponding action error; selection-generation checks still discard late responses.

## Evidence

- [Native build](process-logs-reverification-build.txt) and [CTest](process-logs-reverification-native.txt): 24/24 passed.
- [Console production build](process-logs-reverification-console.txt): passed; existing bundle-size/module-directive warnings remain.
- [Full browser suite](process-logs-reverification-browser.txt): 24 passed, 13 environment-gated tests skipped. Includes both remediation regressions: checksum and network errors persist across successful rendered catalog polls, simultaneous errors stay independent, retry/recovery works, and source changes clear old content/errors.
- [Dedicated Linux lifecycle](process-logs-reverification-live.txt): passed using the newly built console assets. Checks real Docker byte equality, authentication/CSRF/Origin and node isolation, tail rollover, 64 KiB truncation, exact browser download, agent restart, destruction and retained API/console access.
- [Local hashes](process-logs-reverification-local-source.txt) match the [Linux backend/qualification source hashes](process-logs-reverification-linux-source.txt). The backend has not changed since the preceding verification.

Previous [QEMU worker/runner comparisons](process-logs-verification-qemu.txt) and [store/actual ENOSPC tests](process-logs-verification-linux.txt) remain applicable to that unchanged backend; they were not repeated during this UI reverification.

Remaining qualification limits are unchanged: tail-window rollover rather than actual Docker log-driver rotation; injected source-generation transitions rather than a real workload restart; no dedicated stalled-runtime/slow-consumer or power-loss test; empty runner output comparison rather than nonempty stderr. The resolved UI defect does not close those coverage limits.

[Cleanup](process-logs-reverification-cleanup.txt) confirms the temporary services are inactive and no containers, capture/terminal workers or OVS bridges remain. Production source was unchanged during this reverification.
