# Application-edge telemetry v1

This optional extension builds on application dataflow declarations and preserves node-scoped `graphlab.application-telemetry/v1`. LabSupport 1.4.0 validates both formats. Existing workloads and saved node reports need no migration or reporting changes.

## Declarations and identity

An application edge may declare:

```yaml
protocol:
  apiVersion: graphlab.application-protocol/v1
  transport: tcp
  framing: fixed-16
  schema: echo-alpha/v1
```

All three identifiers are required, 1–128 ASCII characters matching `[A-Za-z0-9][A-Za-z0-9._:/-]*`. Unknown fields/versions are rejected. This is declared metadata, not runtime verification. Adding it changes the topology hash; absence preserves the existing canonical form.

The existing identity-checked Docker gate status may include `applicationEdgeTelemetry`, an array of at most four reports. The whole gate response retains its 4096-byte bound; each report retains the 3072-byte bound. Oversized envelopes fail observation before ingestion. No new ingestion listener or credentials are introduced. The agent supplies run, node and immutable container ID (`workloadInstance`). Workloads cannot assert those identities in a report.

Each [report](../schemas/application-edge-telemetry-v1.schema.json) uses `apiVersion: graphlab.application-edge-telemetry/v1`, names a declared `edge`, and states `endpoint: source` or `target`. The agent checks that its actual node is that declaration's endpoint. Both reporters use the same edge ID and their own endpoint role, epoch and stream label. Source and target clocks/sequences are independent.

One series exists per run/node/edge/endpoint. Stream labels cannot change while the instance baseline is retained. Distinct streams need distinct declared edges; parallel edges may share the same network associations. Duplicate endpoint entries in one status are rejected. Reports are committed individually; a later rejected report does not roll back earlier accepted series, and the node's error makes its current observations stale.

## Measurement semantics

Epoch, sequence, elapsed time, counters, histograms and rates preserve v1's decimal uint64, monotonic interval and cumulative histogram semantics. Same-sequence changed reports, regressions and within-epoch changes in metric availability are rejected. Exact duplicates neither append history nor renew freshness. Workload instance replacement discards the old rate baseline even if its reported epoch is reused. Collector restart, first sample and intervals over 15 seconds have unavailable rates.

Source reports own sent-message and sent-payload counters. Target reports own received-message and received-payload counters. The other endpoint's counters are **null**, not zero. Local errors/rejections belong to the reporting endpoint. No source/target totals are summed. Node aggregate reports remain separately labeled and are never added to edge reports.

`reconnects`, `backpressureEvents`, and `backpressureNs` may be null when unsupported/unmeasured. Otherwise they are cumulative decimal uint64 values. Zero means the reporter measures the metric and has observed no such events. This extension does not fabricate reconnect metrics for UDP.

Latency remains `local-service-time`, measured from complete receive to successful echo-send completion on the same process steady clock. For target reports, histogram count cannot exceed received messages. The fixture measures only successful echoes; rejected/failed responses do not contribute latency samples. This includes local processing, socket readiness waits and send calls; it excludes receive wait, peer processing and network transit. No synchronized host clocks are assumed. RTT and one-way delay remain unsupported.

Histograms are process-cumulative, with disjoint upper bounds 10, 50, 100, 500, 1000, 5000 and 10000 microseconds plus overflow. The displayed p95 is a bucket upper bound, with explicit overflow and sample count. No cross-reporter histogram aggregation or rolling-window percentile is performed.

## Persistence and queries

The existing application latest/history tables are reused. Legacy series retain their node key; edge series use a private node/edge/endpoint composite key, while the envelope exposes the actual node. No SQL schema migration or history rewrite is required. Reports retain their workload instance in the envelope.

All node and edge series share limits: 1000 latest records, 10000 history records, 24-hour history retention, 4 MiB latest encoded bodies and 32 MiB history encoded bodies. New envelopes are bounded to 4096 bytes. Pruning is transactional with report insertion; byte pruning removes oldest batches of 100. Logical body limits do not cap SQLite's main file/WAL or guarantee filesystem capacity. Optional collection failures are visible without altering execution/capture.

Existing authenticated application-telemetry GET/query routes remain. POST query now accepts optional `edge`; it intersects with `node` and the agent-owned run. Without an edge filter, queries retain their v1 node-only results, avoiding mixed report versions for existing clients. Unknown edge IDs return no reports, not a fallback to node/interface metrics. Limit stays 1–200; history is a newest-report window, not stable cursor pagination. Response includes shared row/byte limits. Existing Origin/CSRF and run existence checks apply. Staleness and retained reads after teardown are preserved.

The application view displays declared protocol metadata. Selecting an application edge scopes the application panel to that edge, retaining separate endpoint reporters and exact counters. Source/target inspection returns to node scope. Network associations and interface badges do not allocate shared link counters to application flows.

## Two-stream qualification fixture

`docker-nodes/app-streams` is a separate, explicitly opt-in fixture using the five-argument lifecycle overload. It retains the UDP node report and adds a TCP listener on data0:49001, gated by the existing release/traffic lease. Quiescence/lease expiry closes both listeners. The SDK's ordinary app-a overload remains UDP-only; app-b remains uninstrumented.

Each accepted TCP connection carries one fixed 16-byte record. Sixteen `A` bytes select edge/stream `alpha`; sixteen `B` bytes select `beta`. Both are target reports; declarations must name this workload as their target. An accepted complete record counts one received message and 16 payload bytes. Invalid records with a recognized first byte count as rejected. Unknown tags and incomplete records cannot be attributed to either stream and are excluded. Input/output waits are bounded to 500 ms each; the fixture is a qualification tool, not a general TCP stream processor.

Reconnects count recognized complete-record connections after the first connection in each stream during the process epoch. They are not failed connect attempts, TCP retransmissions or proof of recovered delivery. The fixture does not measure backpressure counts/duration and reports null. Errors count failed echo writes after a recognized complete record. `control probe-alpha IP` and `control probe-beta IP` explicitly generate one exchange from an uninstrumented app-b built with SDK 1.4.0, and verify the exact echoed record. Collection itself generates no traffic.

Limitations: Docker collection only, four bounded reports per status, one stream per edge endpoint, shared serialized five-second collector, cumulative local latency, no source endpoint instrumentation in this fixture, no message IDs, delivery-loss inference, packet correlation or source pause/resume. QEMU reporting and general telemetry push ingestion are not added.

## Traffic-lease deadline enforcement

TCP connection waits and fixed-record reads/writes use the earlier of their 500-ms I/O timeout and the active absolute monotonic traffic deadline. Readiness is rechecked against that deadline after each wait and before each nonblocking I/O call. Expired or incomplete exchanges close their scoped peer socket; the gate expires before handling subsequent UDP traffic or producing a control response. Unleased development release retains the independent 500-ms timeout. This prevents initiating new application sends after the observed deadline; it does not retract bytes already handed to the kernel or promise when previously queued TCP traffic reaches the wire.

Portable socket tests cover delayed/partial input, saturated send buffers, expired writable sockets and ordinary timeout behavior. The Linux namespace fixture reproduces the original 10-second boundary against the installed-SDK app-streams binary: `sudo unshare --mount --net --fork sh tests/qualification/application_lease_linux.sh /absolute/path/to/lab-node /absolute/path/to/tests/qualification/application_lease_linux.py`. The wrapper refuses host mount/network namespaces and cleans up its process and private interfaces.
