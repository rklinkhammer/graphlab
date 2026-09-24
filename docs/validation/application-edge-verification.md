# Increment A verification — 23 September 2026

**Historical finding, now remediated.** This verification reproduced one traffic-lease defect; the subsequent remediation fixes it and adds portable and real Linux boundary regression tests. See [remediation results](../web-console-feature-parity.md#increment-a-traffic-lease-remediation--23-september-2026). The original reproduction below is retained as evidence.

## Fresh checks

- Native build: passed; CTest 21/21 passed (18.79 seconds).
- Console production build: passed with existing Vite notices.
- Browser suite: 16 passed, 11 opt-in live cases skipped (37.3 seconds).
- Dedicated Linux application contract/store executable: passed, including ownership, independent streams, nullable counters, exact deltas, restart, bounds and transaction rollback.
- The full live API/browser traffic scenario was not rerun. Its prior evidence remains separate.
- Additional isolated Linux traffic-lease boundary test: **failed the required invariant**.

## P1: TCP echo can transmit after traffic lease expiry

`packages/lab-support/cpp/lifecycle.cpp:219` reads a record with a separate 500-ms I/O deadline, then sends its echo at line 230 without rechecking the traffic lease. `record_io` (line 78) does not know the lease deadline. The outer loop's expiry checks do not run while this work is in progress.

Reproduction used the existing SDK 1.4.0 app-streams binary inside fresh mount/network namespaces on the dedicated Linux VM. `/run` was a namespace-private tmpfs and `data0` a namespace-private dummy interface. No production run, capture process, host socket or network resource was changed.

1. Issue `release-lease` (10 seconds).
2. Connect to TCP port 49001 at approximately 9.80 seconds, without sending a record yet.
3. Send 16 `A` bytes at approximately 10.15 seconds.
4. Observe a complete echo after expiry, followed by gate status `held`.

Actual output:

```json
{"secondsAfterRelease":10.153,"echoAfterLeaseDeadline":true,"echoBytes":16,"statusAfterEcho":"held"}
```

The Linux fixture source and workspace source differ only in formatting: formatting the Linux source with the workspace's clang-format produced an exact match. This is a current implementation defect, not merely an old binary discrepancy.

The fixture can continue generating traffic after its authorization expires, undermining the preserved traffic-lease/capture-first boundary. The existing normal-traffic and quiesced-restart tests do not exercise a partial/delayed record crossing expiry.

Required correction: propagate the active traffic deadline through accepted-peer reads/writes and TCP probes; stop processing and close the peer when authorization expires, and recheck after waits before sending. Add boundary tests for delayed/partial records and blocked writes, and verify that no payload is emitted after expiry. Keep the ordinary I/O timeout as a separate upper bound.

Reproduction sources: [Python driver](application-edge-lease-repro.py), [namespace wrapper](application-edge-lease-repro.sh). These reference the isolated Linux build and expect the driver at `/tmp/verify-edge-lease.py`; run only under `sudo unshare --mount --net --fork` as shown in the wrapper's qualification usage. The wrapper mounts a private `/run` and must not be invoked directly on the host. The process was terminated by the wrapper's EXIT trap; the namespaces were destroyed. Docker and OVS inventories remained empty.
