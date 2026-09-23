# M5 telemetry and directional faults

M5 adds a C++ collector, SQLite history, shared rate derivation, typed netem jobs and a browser telemetry/fault/timeline panel. Start the existing Linux agent/API as described in [M4 operations](m4-qemu-consoles.md); open a ready run and select an edge in **Directional telemetry and faults**. Preview placement before applying a fault. The panel exposes the actual interface, namespace identity, direction and qdisc handle. Active faults can be removed explicitly; queued/running apply jobs can be cancelled.

## Observations and direction

The Linux backend reads rtnetlink `stats64` through `ip -j -s -s link`, after checking the journaled interface alias/ifindex and, for Docker, container identity and network namespace inode. It batches interface dumps per namespace. One canonical endpoint supplies both directions for each logical edge; endpoint counters are never added together. A→B follows the topology's ordered endpoints. For veth endpoint A, TX is A→B; at endpoint B, RX is A→B. For a TAP, RX is guest→switch and TX is switch→guest. These are software-interface counters, not wire utilization or application goodput. See the [kernel statistics documentation](https://docs.kernel.org/networking/statistics.html).

The independently installable `LabSupport::telemetry` target provides rate/epoch derivation without privileged runtime dependencies. Raw counters, boot identifiers, epochs and monotonic nanoseconds remain strings in JSON. Rates use integer deltas before floating-point conversion. First samples, invalid observations, counter decreases, identity changes, collector restarts and intervals longer than three seconds produce null rates and explicit gaps. Charts break at missing intervals and aggregate gaps. No missing rate is represented as zero.

The worker targets one-second interface/qdisc observations, caches OVS RSTP observations for two seconds, and checks resource health every five seconds. Each batch retains its counter-read monotonic timestamp. Samples are stale after three seconds; browser poll failure also ages the display. Collection duration is exposed as `lastCollectionDurationNs`. Sampling shares the serialized executor: long operations can delay polling and create visible gaps. This is a small-lab implementation, not a scale or real-time qualification. Reduced-observation profiles and separate capture-overhead accounting remain future work.

History uses one-second buckets for one hour, ten-second summaries for one day and minute summaries for seven days. Summaries contain mean valid rates, sample/valid/gap counts, and the latest raw observation. Means are not time-weighted traffic integrals. A global 100,000-row ceiling across all runs can shorten these windows. Query responses expose oldest/newest retained buckets and point-limit truncation. A query returns at most 2,000 points; select an edge to avoid sharing that budget across edges. The timeline retains 4,096 observation/fault events per run plus the existing bounded job/capture/session records; its browser view shows the latest 100 entries. UTC provides correlation, while boot/monotonic fields identify elapsed-time context.

## Fault contract and recovery

Only the typed `netem` operation is admitted: edge, `a-to-b` or `b-to-a`, integer delay 0–5,000 ms, integer loss 0–100%, and duration 1–3,600 seconds. Delay or loss must be nonzero. Unknown fields and arbitrary commands are rejected. Admission requires an available run, current revision and a principal-scoped idempotency key. Concurrent mutations serialize; another live fault in the same edge direction conflicts.

The supported placement is an owned egress qdisc: Docker/switch veth directions and switch→guest TAP traffic. **Guest→switch TAP faults are explicitly rejected (`guest_egress_fault_requires_ifb`); IFB ingress redirection is not implemented.** Other netem options and node-failure faults are not admitted. Preview and apply recheck mapping identity. A foreign root qdisc is never replaced. The C++ executor calls fixed `tc` argument vectors, reads back the owned handle, and retains observed qdisc details. See [netem semantics](https://man7.org/linux/man-pages/man8/tc-netem.8.html).

Intent is saved before effects. Duration starts after successful activation/readback and uses a monotonic deadline plus boot ID. Normal expiry removes the owned qdisc. Same-boot recovery preserves an unexpired deadline; expired, incomplete or different-boot intent is removed before traffic can resume. Removal failures durably mark the fault `recovery-required` and the run `reconciling`, then explicitly quiesce the owned run, including no-lease development runs. The journal and timeline record the request and acknowledgement or failure. Explicit remove/recover or restart retries cleanup; traffic is not automatically resumed. A quiescence failure remains visible and requires operator recovery. Cancellation during apply compensates by removing the qdisc. Run cleanup clears faults before deleting interfaces.

Expiry depends on the agent executor: an outage or long-running job can delay removal. Restart and resume reconcile deadlines; this is not an independent kernel timer or hard maximum fault-duration guarantee. Readback proves handle/kind presence and records options; exhaustive semantic comparison of every kernel netem option and hostile replacement using the same handle are not qualified.

## API and CLI

Existing authentication, origin/CSRF checks and local RPC principal checks apply. Under the existing API prefix:

| Route | Purpose |
|---|---|
| `GET runs/{id}/telemetry` | Current observations and default recent history |
| `POST runs/{id}/telemetry/query` | Bounded history query, with CSRF token |
| `GET runs/{id}/faults` | Fault records and current revision |
| `POST runs/{id}/faults/preview` | Read-only placement preview |
| `POST runs/{id}/faults` | Apply job |
| `DELETE runs/{id}/faults/{faultId}` | Remove job; revision/key in JSON body |
| `POST runs/{id}/faults/remove` | Browser-compatible remove job alias |
| `GET runs/{id}/timeline` | Correlated events |

History query fields are `windowSeconds`, `edge`, `resolutionSeconds` (1/10/60), `fromUnixSeconds`, `toUnixSeconds` (decimal strings) and `limit` (1–2,000). Maximum ranges are one hour/day/week respectively. The browser uses `windowSeconds` (1 through the resolution’s maximum range); both endpoints are derived from one server timestamp, independent of browser clock skew or network delay. Do not mix `windowSeconds` with explicit `fromUnixSeconds`/`toUnixSeconds`. Responses include the resolved endpoints. History, fault and timeline polling update independently; history failure ages rates using the browser’s monotonic clock and does not suppress fault/timeline updates. GET query-string filters are not implemented; use the POST query route.

Apply body example (replace the revision and edge):

```json
{"expectedRevision":"1","idempotencyKey":"example-fault-001","fault":{"kind":"netem","edge":"a-s","direction":"a-to-b","delayMs":100,"lossPercent":0,"durationSeconds":10}}
```

The existing `lab control --socket PATH --agent-uid UID METHOD PARAMS.json` command exposes methods `telemetry`, `timeline`, `faults`, `fault.preview`, `fault.apply` and `fault.remove`; params also include `runId`. See `lab --help` for transport invocation. These paths use the same executor and revision/idempotency checks as the browser.

Acceptance commands and measured limits are recorded in [M5 verification](validation/m5-verification.md). No Python component is required.

M6 adds an explicit `minimal` observation profile for new runs; fault expiry and capture/guest safety enforcement remain enabled. See [M6 qualification](m6-qualification.md) for the measured baseline comparison and profile contract.

## Topology-linked performance and history

The graph and run panels share the selected edge. Edge badges show A→B/B→A rates from the same bounded query used by the inspector; they age to stale on failed polling. Link performance includes packet rates and exact decimal byte/packet/error/drop counters, with the canonical endpoint's RX/TX mapped to topology direction. Charts have UTC axes, point values, a bounded value table, and breaks at missing observations. The display counter baseline affects only the browser and becomes invalid on counter-epoch changes; it does not reset interface counters or delete history.

Graph selection also filters the capture catalog and chooses the fault edge. Run and artifact selectors are restricted to the selected topology revision. Inspecting a different retained run disables the active run's lifecycle controls until it is selected again. Workspace navigation jumps to performance, consoles, history, and captures without terminating sessions.

The correlated timeline supports filtering, older-record pages of 100 items, and return-to-newest. Paging freezes the returned event snapshot so background polling cannot move the page. Pagination is within the backend's bounded retained event response, not a packet-history database. Application latency, backpressure, message identity, and per-packet observations remain unavailable without an additional workload/capture measurement contract.
