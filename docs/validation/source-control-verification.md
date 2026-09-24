# Increment B verification — 23 September 2026

**Historical finding, now remediated.** This verification reproduced a P2 console recovery defect. The subsequent [remediation](../web-console-feature-parity.md#increment-b-console-recovery-remediation--23-september-2026) fixes it with rejection/uncertainty browser regressions and a real Linux API rejection test. No implementation fix was made during the original verification pass.

## P2: rejected commands strand the source controls

In `console/web/src/source-controls.tsx:22`, a command rejection clears `pending` but leaves `retry.current` and the `requested` outcome intact. At line 13, the next successful status poll clears the error. An admission rejection has no retained command record, so the code never clears the retry reference. Lines 30–31 disable every source button while that reference exists; line 33 hides the retry button once polling clears the error.

Reproduced with HTTP 409 `source_epoch_changed`, an otherwise successful capability query and an empty command list. After the next 1.5-second poll, both source actions remained disabled, the error/retry button disappeared, and the outcome still said `requested`. This can occur when a process epoch changes between observation and submission. Capacity or other admission rejections can reach the same path. Switching to another node and back remounts the panel as a workaround.

Required correction: distinguish command errors from query errors; represent definitive admission rejection as a failed outcome and allow a fresh identity-bound command after refreshing capability. For uncertain transport failures, retain an explicit reconciliation/same-request retry path that successful polling cannot hide. Preserve selection fencing and do not silently resend against a replacement workload.

[Reproduction output](source-control-verification-rejection.txt) asserts the observed defect, so its passing test means **the bug reproduced**. The [reproduction snippet](source-control-rejection-repro.ts) can be appended temporarily to `console/web/tests/parity.spec.ts` and run with `npm test --prefix console/web -- --grep 'verification: rejected source'`. Add a regression asserting correct recovery when fixing this behavior. The temporary verification test was removed after recording evidence.

## Fresh checks

- Native build and **23/23 CTest cases passed** (21.06 seconds). [Log](source-control-verification-native.txt).
- Console production build passed with existing Vite notices; default browser suite **17 passed, 12 opt-in live cases skipped** (37.5 seconds).
- Dedicated Linux source-control contract/runtime tests passed. [Log](source-control-verification-linux.txt).
- SHA-256 matched local/Linux copies of `source_control.cpp`, `linux.cpp`, `process.cpp`, workload `lifecycle.cpp` and `source_control.hpp`, native source tests and the console source-controls component.
- Fresh dedicated Linux capture-first API/browser scenario passed (16.9 seconds): alpha stayed at 12 while beta advanced 16→27; independent verified PCAPNG records strictly inside the pause interval contained zero alpha and ten beta datagrams. Capture processes were unchanged, receiving continued, alpha resumed, run quiescence stayed authoritative, and retained outcomes survived agent restart. [Log and exact artifact/block references](source-control-verification-live.txt), [panel](source-control-verification-live.png).
- All task fixture runs were destroyed; task services stopped; running Docker and OVS inventories empty. [Cleanup](source-control-verification-cleanup.txt).

The existing native/default browser/live success-path coverage did not detect the rejected-command recovery defect. Previously documented limits (Docker opt-in, bounded lifetime retention, no new QEMU/power-loss/ENOSPC qualification) remain unchanged.
