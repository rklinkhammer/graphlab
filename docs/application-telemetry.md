# Application telemetry v1

Application telemetry is separate from interface telemetry. It describes workload-reported messages, payload bytes and local processing measurements. It does not imply packet delivery, application-edge discovery, wire bandwidth, RTT or one-way network latency.

## Reporting and trust

The optional `applicationTelemetry` member of the existing `graphlab.gate/v1` status response carries `graphlab.application-telemetry/v1`. SDK 1.3.0 exports `lab_support/application_telemetry.hpp` through `LabSupport::telemetry`, and adds an opt-in lifecycle overload. `app-a` enables reporting when built with SDK 1.3; `app-b` and older images continue to work without reports. Gate major/minor and required features are unchanged. Optional capability `application-telemetry/v1` advertises reporting.

There is no public report-ingestion HTTP route, network listener, host socket mount, or new workload credential. Every five seconds under the full observation profile, the agent reads the fixed status command from a Docker container whose run/resource labels and immutable identity are checked. Run/node identity comes from the agent's resource journal, never the report. The application can misreport its own measurements; identity checks establish provenance, not measurement truth. QEMU and external workloads are not instrumented by this increment.

A status response is bounded to 4096 decoded bytes before JSON parsing; a report is bounded to 3072 bytes. Unknown fields/versions and malformed counters/histograms are rejected. A failing Docker observation is isolated to that node. Errors are visible in the application API, and previously accepted data is marked stale when its current observation fails. The minimal observation profile disables collection. Collection shares the existing serialized executor and can be delayed by operations; it is not real-time telemetry.

## Wire contract and counters

The [schema](../schemas/application-telemetry-v1.schema.json) documents the format; shared C++ validation additionally enforces uint64 limits and cross-field consistency.

- `apiVersion`: exactly `graphlab.application-telemetry/v1`.
- `stream`: one named stream per node, 1–32 lower-case identifier characters. This increment uses `udp-echo`; streams are not inferred topology edges.
- `epoch`: 128-bit random process-start identifier, encoded as 32 lowercase hex characters.
- `sequence`: positive increasing report sequence, as a decimal string.
- `elapsedNs`: cumulative process monotonic elapsed time, as a decimal string.
- `counters`: cumulative decimal-string uint64 values for `sentMessages`, `receivedMessages`, `sentPayloadBytes`, `receivedPayloadBytes`, `errors`, `rejectedMessages`, and `backpressureEvents`.
- Optional `latency`: `kind: "local-service-time"`, decimal `count`, `sumNs`, and eight disjoint decimal-string histogram bucket counts.

Leading zeros, signed values, floats and overflowing integers are rejected. Counters cannot decrease within an epoch. A report sequence cannot move backward. Exact duplicates do not append history or renew freshness; a changed payload using the same sequence is rejected. A new workload epoch or collector process creates a rate gap. Intervals over 15 seconds also create a gap. Valid rates use exact integer deltas before floating-point conversion, divided by the workload's monotonic interval; no host wall-clock subtraction is used. Zero traffic in a valid interval is zero; unavailable rates are null.

## Fixture measurement semantics

The instrumented echo fixture counts data0 UDP datagrams successfully returned by `recvfrom`, and bytes in those payloads. Successful `sendto` calls count echoed messages and payload bytes; failed sends increment errors, and EAGAIN/EWOULDBLOCK also increment backpressure. Oversized datagrams beyond the 1500-byte echo buffer are counted received but rejected without a truncated echo. This rejection behavior applies to the instrumented fixture; uninstrumented fixture behavior is preserved. The fixture does not count control-plane probes as its own echo-service messages and generates no traffic merely to report status.

Latency starts immediately after a successful receive syscall returns and ends immediately after the corresponding successful echo send syscall returns, both on the same process's `steady_clock`. It includes local userspace processing and the send syscall. It excludes time waiting to receive, network transit, peer processing, and lost/failed echoes. There is no cross-host clock-synchronization assumption. This is **local echo-service time**, not RTT, one-way network delay or end-to-end application latency.

Bucket upper bounds are 10, 50, 100, 500, 1000, 5000 and 10000 microseconds, plus overflow. Histograms and mean are cumulative within a workload epoch, not a rolling interval. Mean is `sumNs/count/1000`. P95 uses rank `ceil(0.95 * count)` and returns the containing bucket's upper bound, not an exact percentile. An overflow p95 returns a null upper bound and an explicit `p95Overflow: true` (>10000 µs), rather than clamping to 10000. No samples yields null mean/p95. Histogram counts, sum ranges and sent-message count are validated for consistency.

## Storage and API

SQLite in the existing private agent state directory stores current reports and history atomically. Logical retention is bounded to 1000 latest node/run records globally and 10000 history records globally; history also expires after 24 hours. Pruning occurs during ingestion and history queries exclude expired rows. Latest rows remain as stale retained snapshots until evicted. Physical SQLite/WAL allocation can retain reusable pages; these logical ceilings are not filesystem quotas. Input size limits bound individual records. The latest-report baseline survives agent restart, while the new collector epoch prevents a false uninterrupted rate. Timestamp `observedAt` is agent UTC for correlation, not a latency clock.

Authenticated routes:

- `GET /api/v1/runs/{id}/application-telemetry`: current records and up to 100 recent history records.
- `POST /api/v1/runs/{id}/application-telemetry/query`: same data, optional `node` and `limit` (1–200). Requires existing Origin and CSRF checks.
- Existing CLI/RPC method `application-telemetry`, with `runId` and optional query fields, uses the same agent-owned database.

Responses distinguish `current` from newest-first historical `items`, report truncation and retention bounds, and expose node/collector errors. Staleness uses the agent's monotonic receipt age (15 seconds) and collector identity; destroyed runs are always retained/stale. The browser additionally ages failed requests and suppresses current rates/latency when stale. Cumulative raw counters and historical observations remain inspectable and labeled as such.

## Reference and scope

Source comparison used graphx-docker commit `7cad4da8646eda302a005070228495c1aa87d89a`, particularly `apps/telemetry/metric-store.mjs` (`ingestTotals`, `currentRates`, `latencyPercentile`) and `web/src/components/EdgeInspector.jsx`. This implementation preserves the separation of cumulative application counters and unavailable measurements, while using explicit local-service semantics, decimal uint64 counters, strict histogram validation and honest overflow bounds. It does not copy GraphX's five-second event-rate window or assume GraphX message envelopes.

Message/trace identities, exact capture correlation, reconnect metrics, delivery loss, multiple streams per node, source controls, application-edge mapping and packet history remain outside this increment. No active probes are launched by the collector; verification explicitly invokes existing fixture probes.

## Compatible edge-report extension

[Application-edge telemetry v1](application-edge-telemetry.md), added in SDK 1.4.0, preserves this report format and the default node-only query results. Explicit edge queries return separately versioned endpoint-owned reports. Both share the existing store with additional 4 MiB latest / 32 MiB history encoded-body ceilings. See that contract for identities, measurement ownership, fixture semantics and limitations.
