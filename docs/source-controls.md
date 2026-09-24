# Source controls v1

LabSupport 1.5.0 adds opt-in `graphlab.source-control/v1` through the existing identity-checked Docker gate. Whole-run quiesce/resume is unchanged. An ordinary echo receiver, QEMU guest or legacy workload advertises no source controls.

The `app-sources` C++ fixture generates two independent UDP datagram streams, `alpha` and `beta`, targeting the fixture-configured `10.231.17.1:49000` through `data0`. Each datagram is exactly sixteen repeated `A` or `B` bytes. A best-effort 200-ms tick attempts one datagram per enabled source; missed ticks are not replayed. The target is compiled fixture configuration, not an API-supplied address. `app-a` remains the UDP receiver. Declare alpha/beta application edges from the generator node to the receiver, with UDP protocol metadata.

## Semantics and capability

Gate status advertises `source-control/v1` and a `sourceControl` object with `apiVersion`, a random 32-lowercase-hex process `epoch`, and at most four sources. Each source has `id`, `state` (`running` or `paused`), and decimal-string `generatedDatagrams`. The latter counts successful full datagram send syscalls; it is neither delivered messages nor interface packet counts. There is no message identity or packet correlation contract.

Pause disables **new generation by that source**. Other sources, UDP echo reception, draining replies, and the management/control socket continue. The single-threaded fixture acknowledges after earlier send syscalls complete; already queued kernel/network datagrams cannot be withdrawn. No future send is authorized by the paused source until a new resume. Resume changes the source preference only: effective generation also requires the whole-run gate released and, where required, its traffic lease valid. Run quiescence/lease expiry stops all generation. Source preference survives run quiesce/resume and agent restart while the workload survives. A replacement workload has a fresh epoch and starts with default running preferences behind the held startup gate.

## Authenticated API

Both routes require the existing session, allowed Origin and CSRF token:

- `POST /api/v1/runs/{runId}/source-controls/query`, body `{"node":"b"}`: current advertised capability, immutable container `instance`, run state, and retained command outcomes. Missing/unsupported capabilities are null with an explicit error; no controls are fabricated. Retained outcomes remain readable after destruction.
- `POST /api/v1/runs/{runId}/source-controls/command`:

```json
{"apiVersion":"graphlab.source-control/v1","node":"b","instance":"<64-hex-container-id>","epoch":"<32-hex-process-epoch>","source":"alpha","action":"pause","requestId":"unique-request-1"}
```

The run comes from the route. The agent verifies the journaled container identity, advertised source/epoch, and declared application-edge source ownership. Reconciliation/destroyed runs and concurrent run mutations reject new commands. Browser callers cannot provide commands, executable paths, Docker IDs unrelated to the owned resource, or destinations. Source resume is allowed while stopped but cannot release the run.

The workload wire command is `source-command ` followed by JSON containing only `apiVersion`, `epoch`, `source`, `action`, and `requestId`. An acknowledgement echoes this exact object with `outcome: acknowledged`. The immutable container check fences replacement containers; the process epoch fences restarts inside the same container. Neither agent nor workload reapplies a retained duplicate, including a delayed duplicate pause after a later resume.

## Bounds, outcomes and recovery

The existing serialized executor owns dispatch; there is no new worker or capture process. At most eight commands are requested globally and one per run/node/source. Normal run operations and health/lease monitoring have precedence. Queue admission has a ten-second monotonic deadline; expired commands are not sent. Docker transport retains its existing bounded calls, and the gate client has a 2.5-second response deadline. Transport failures may leave application state uncertain; a timeout is not proof of nonexecution.

Outcomes are `requested`, `acknowledged`, `failed`, `timed-out`, or `interrupted`. Requested state commits to the existing SQLite journal before dispatch; final outcome commits afterward. Agent startup marks unresolved requested records interrupted and **never replays them**. Query fresh workload status to reconcile an uncertain outcome; issue a new explicit request only after inspecting current identity/state. Repeated request IDs return the original record, or conflict if any bound parameter differs.

Request IDs are 1–64 ASCII letters/digits/underscore/hyphen. Requests are at most 1,024 encoded bytes; each string is at most 128 bytes. Capability data is at most 2,048 bytes; errors retained in command outcomes are at most 256 bytes. The unchanged gate response limit is 4,096 bytes. The agent retains at most 256 command records for the state-directory lifetime (under 512 KiB of encoded command records and the existing 16-MiB state journal limit). The workload retains at most 256 commands for its process lifetime. Capacity rejects new commands instead of silently evicting idempotency protection. There is no automatic expiry, archival or in-place ledger reset in this increment; this is a bounded small-lab control implementation, not unlimited command history.

Console controls appear only for advertised sources on the selected workload node. The target container is shown with source state, generated datagrams, pending/final outcome and retained records. Selection changes fence late query/command responses. A transport error offers retry using the **same** request identity. Current query failure clears controls rather than leaving stale capability buttons enabled.

## Reference and qualification

Reference inspection at `7cad4da8646eda302a005070228495c1aa87d89a`: `apps/telemetry/control.mjs` (`ControlPlane.issue`, `acknowledge`, maintenance) implements actor-scoped idempotency, bounded command/audit collections, explicit acknowledgement and timeout. Graphlab uses its owned gate/runtime transport and durable state journal; stream-specific control, process-epoch fencing and preservation of capture-first admission are explicit Graphlab extensions. This is source inspection, not runtime qualification of graphx-docker.

Fresh verification and limitations are recorded in the [parity report](web-console-feature-parity.md). QEMU source control, arbitrary generators, source configuration from the browser, delivery accounting, application-message correlation and durable process logs are outside this increment.


### Console rejection and uncertain-result recovery

HTTP admission rejections (400/401/403/404/405/409/422/429) produce a visible local `failed` outcome. The command error remains visible through successful status polling. Controls require a capability query started after the rejection before accepting a fresh action; that action uses a new request ID and the newly observed instance/epoch. Queries already in flight cannot re-enable controls with pre-rejection identity.

Network failures, timeouts and server errors remain locally `unknown — awaiting reconciliation`. The console retains the exact request body/ID, blocks new source actions, and keeps **Retry same request** available even when status polling succeeds without a matching retained record. A terminal retained outcome or terminal command response resolves uncertainty. Retrying never substitutes a newly observed workload identity into the old request. Selection changes continue to fence late responses.
