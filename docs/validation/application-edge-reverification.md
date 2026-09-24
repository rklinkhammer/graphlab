# Increment A re-verification — 23 September 2026

**Passed within the documented increment A scope.** The previously reproduced traffic-lease blocker did not recur. No implementation changes were made in this verification pass.

## Fresh checks

- Native build succeeded; CTest **22/22 passed** in 17.99 seconds, including `application_traffic_lease`. [Full output](application-edge-reverification-native.txt).
- Console production build succeeded with existing Vite notices. Default Playwright suite: **16 passed, 11 opt-in live tests skipped**, in 36.7 seconds. This includes edge selection, independent endpoint counters, unavailable metrics, legacy topology compatibility and stale telemetry presentation.
- Dedicated Linux ARM64 application telemetry contract/store tests passed, including ownership, independent streams, exact counters, restart recovery, bounded persistence and transactional rollback.
- Portable lease socket tests also passed on Linux, including expired writable sockets, delayed/partial records, blocked writes, expired readiness and unleased timeouts.
- Real SDK fixture in isolated Linux mount/network namespaces passed both expiry regressions and subsequent unleased alpha/beta exchange and quiescence:

```json
{"partial": false, "elapsedSeconds": 10.156, "echoBytes": 0, "state": "held"}
{"partial": true, "elapsedSeconds": 10.152, "echoBytes": 0, "state": "held"}
```

The regression driver also checks that the expired records do not increase the accepted alpha message count. SHA-256 comparisons matched workspace and Linux copies of `lifecycle.cpp`, `record_io.hpp`, `application_lease.cpp`, and both Linux qualification scripts. Linux source/build remains `/tmp/graphlab-edge-20260923`.

After the checks, both isolated agent/API services were inactive; running Docker containers and OVS bridge inventories were empty. The namespace wrapper terminated its fixture process and removed its private network/mount namespace resources. `git diff --check` passed.

## Scope and remaining limits

The full capture-first Docker/OVS → API → browser scenario was **not rerun in this pass**; its successful post-remediation evidence remains in [the remediation live log](application-edge-remediation-live.txt). This pass freshly verifies the default browser suite and the actual Linux lease boundary. It does not claim new QEMU, saturation, power-loss or application-message correlation qualification.

No additional blocking defect was identified in the reviewed lease paths. The deadline bounds authorization of new application I/O; it cannot retract bytes already queued in the kernel or guarantee wire transmission time. Existing increment A limitations, including target-only fixture instrumentation and unavailable backpressure measurements, remain. Increments B–F are outside this verification.
