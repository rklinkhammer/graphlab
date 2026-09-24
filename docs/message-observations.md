# Application message observations v1

Increment E implements opt-in **GLM1 UDP fixture** observations, durable history and evidence-backed capture correlation. Messages, packet records and aggregate/interface counters remain separate. No delivery/loss accounting or network latency is inferred.

## Reporter contract and identity

LabSupport 1.6.0 preserves the existing lifecycle overloads. The new final `message_observations` boolean opts in; the default is false. `app-messages` generates requests and `app-message-target` echoes them. The advertised `message-observations/v1` capability enables a separate bounded `message-observations` gate command. Ordinary status remains compatible. Legacy workloads without the capability produce no observations and remain operational.

The report is `graphlab.message-observations/v1`: `epoch` (32 lowercase hex), `clock: reporter-monotonic-ns`, `payloadRecorded: false`, decimal-string `total` and `evicted`, and up to eight `events`. Each event has decimal-string `sequence`, `payloadLength: "53"`, `timestampMonotonicNs`, 48-hex `messageId` and `traceId`, `stream` (`alpha`/`beta`), `kind` (`send`/`receive`) and `phase` (`request`/`response`). Unknown fields/versions fail closed. The report is at most 3,500 serialized bytes inside the existing 4,096-byte command envelope. Numeric strings are canonical nonnegative decimal values of at most 18 digits. Events must form the contiguous suffix `evicted+1..total`, with nondecreasing timestamps. See the [structural schema](../schemas/message-observations-v1.schema.json); C++ additionally checks cross-field/window invariants.

The collector supplies trusted run ID, logical node and verified immutable container ID. The source key combines those identities with the reporter epoch; workloads cannot choose another run. Streams are reporter-local fixture labels, not automatic application-edge mappings. Retained event IDs are database IDs, distinct from reporter sequence and message IDs.

GLM1/v1 is exactly 53 UDP payload bytes: `GLM1`, 32 lowercase hex origin-epoch bytes, 16 lowercase hex message-number bytes, then `A`/`B` for alpha/beta requests or `a`/`b` for responses. The 48-hex origin+number is the message ID; trace ID is the same identifier for this simple request/echo exchange. Each source attempt allocates a new ID. The target preserves the ID and changes only phase. There are no automatic retries, child spans or fan-out identities. Repeated sends/retries/reuse of an ID are separate observed occurrences; multiple captures are never collapsed into one delivery. Random process epochs provide practical uniqueness, not an assertion that malicious reporters cannot reuse IDs.

A send event means the local socket accepted the datagram; receive means the workload received a complete supported datagram. Neither proves end-to-end delivery. Timestamps use the reporter's monotonic clock and are meaningful only within its process epoch. They are not synchronized with other reporters or capture wall clocks; no one-way, round-trip or network latency is computed. No payload is persisted in message history. Existing PCAPNG captures retain their normal packet bytes, including fixture identifiers.

## Collection, persistence and omissions

An independent collector sleeps five seconds between bounded batches of up to 16 verified Docker sources, with its own round-robin cursor. Backend calls occur outside the history/engine locks. This is best-effort polling, not a five-second delivery guarantee. The eight-event reporter ring can roll over between polls. `missedBeforeIngestion` counts sequence gaps that were never collected; `reporterEvicted` counts the reporter's total evictions and overlaps already collected events. Do not add these counters or interpret them as packet/message loss.

`messages.sqlite` is mode 0600, uses SQLite FULL synchronous DELETE journaling, transactional insertion/high-water advancement and restart recovery. Retention is global: 4,096 events, 4 MiB serialized event bodies and 24 hours from agent ingestion. Pruning removes oldest events. The database has a 4,096-page ceiling (normally 16 MiB); rollback journals and filesystem overhead require additional space. These limits are not a filesystem quota. Queries also exclude expired records immediately.

At most 256 run/node/container/epoch ledgers exist for the state-directory lifetime. Further reporters are rejected with visible capacity indication; there is no archival/reset UI. High-water marks survive event eviction and restart, so replay cannot resurrect pruned history. Conflicting retained sequence bodies and regressing totals reject the entire batch. Bodies already pruned cannot be compared again. Failed transactions do not advance high-water marks; retry can recover after space becomes available.

The UI exposes retained, missed, reporter-evicted and expired/pruned counts. A reporter is stale after 15 seconds or after a collector restart until observed again. The last batch's bounded collection error is visible but transient and global, not a durable per-source error ledger. Unsupported workloads and runs with no captures are valid and display unavailable data rather than zero measurements.

## Authenticated API and console

Both POST routes require the existing operator session, CSRF token and allowed Origin:

- `/api/v1/runs/{runId}/messages/query`: required `node`, optional `stream` and `cursor`. Returns newest-first pages of at most 20 events plus source coverage. Cursors bind run/node/stream and an exclusive database-ID boundary, so inserts cannot shift older pages. Retention may remove older records; pagination does not pin them.
- `/api/v1/runs/{runId}/messages/correlate`: required `node` and retained event `id`, optional declared network `edge`. Wrong run/node/event scopes reject access. Without an edge the scope is the union of retained run captures.

The separate message panel follows selected node, supports stream filtering, frozen older pages and return-to-newest polling. An explicit capture-edge selector narrows correlation without changing workload selection. Late responses are fenced by selection/page changes. Correlation errors are separate from background history errors. Artifact navigation uses the exact retained artifact ID and displays PCAPNG packet index and block offset.

## Correlation evidence and bounds

Correlation reads existing finalized segments; it launches no capture process and never modifies capture data or the packet-history index. Descriptor/manifest run, capture ID, epoch, edge, mapping and boot identities must agree. Files are opened without following symlinks and size/SHA-256 are verified before decoding. The opt-in decoder recognizes complete, unfragmented IPv4 UDP GLM1 datagrams in Ethernet captures (including supported VLAN framing). Ordinary packet-history queries do not expose message IDs.

Each request scans at most 16 segments, 16 MiB total bytes, 2,000 decoded packets per segment and returns at most 64 matches. Active segments are not read; open captures, truncation, decoding omissions, bound exhaustion and verification failures prevent a uniqueness claim. Matching compares the wire identifier, stream and request/response phase, never a tuple/time heuristic. A single occurrence with complete retained scan and no known interrupted/incomplete run coverage is `exact`; multiple occurrences or incomplete coverage with a match are `ambiguous`; no verified match is `unavailable`. Exact means only one matching occurrence in that retained scope, not globally unique identity, actual route, delivery or endpoint authenticity. Packet timestamps remain capture wall-clock observations.

Unsupported formats, IPv6, fragmentation, TCP/reassembly and encrypted payloads cannot establish correlation. Missing or expired captures cannot establish loss. The API reports scan completeness/errors and run coverage; no capture-retention pinning or automatic backfill is added.

## Qualification and remaining limits

See [increment E evidence](web-console-feature-parity.md#increment-e--explicit-message-observations-24-september-2026). Native tests exercise report validation, replay/conflicts, retention, stable cursor scoping, restart, SQLite transaction interruption, slow backend isolation and correlation integrity/ambiguity. Linux qualification independently compares both workload reports and exact checksum-verified PCAPNG bytes, checks authenticated API/browser behavior, restart/destruction and actual ENOSPC rollback/retry.

This increment supports the two Docker fixture streams only. It does not implement general application schemas, QEMU reporting, automatic application-edge association, reliable event streaming, arbitrary retries/fan-out accounting, delivery deadlines, long-duration saturation qualification or whole-system power-loss durability. Increment F's broader qualification and the earlier increments' documented limitations remain open.

Increment F adds fresh [source/body/expiry/scan-bound, browser and live Linux qualification](validation/increment-f/README.md), including the narrow-screen identifier-wrapping fix. Its report distinguishes newly verified limits from the remaining physical-page and broad saturation qualifications.
