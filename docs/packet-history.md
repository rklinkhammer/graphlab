# Finalized capture packet history v1

Packet history indexes existing, finalized Graphlab PCAPNG segments. It creates no capture process or traffic. It is distinct from the operational timeline and application-message telemetry. The [observation schema](../schemas/packet-observation-v1.schema.json) and this document define the first supported increment.

## Identity and measurement

Each observation includes `apiVersion: graphlab.packet-observation/v1`, agent-owned run ID, capture ID/epoch, edge, capture interface and canonical endpoint, artifact ID and SHA-256, zero-based packet index, and the exact byte offset of its Enhanced Packet Block (EPB). IDs, offsets and timestamps use decimal strings where integer precision matters. An API row also has a monotonically allocated decimal index ID.

`timestampUnixMicros` is the EPB's host realtime capture timestamp, at microsecond resolution. It is not the indexing time, a monotonic interval clock, or proof of clock accuracy. Wall clocks can step; ordering is by index ID, not timestamp. Cross-host clock synchronization is not assumed. The index cannot derive latency or delivery loss.

`capturedLength`, `originalLength` and `truncated` preserve snapshot lengths; truncation is exactly captured length less than original length. Link type is Ethernet (1), PCAPNG interface ID is 0. The writer does not record per-packet direction, so `direction` is always `unknown`. Source/destination IPs and ports describe headers only; they do not establish topology direction or identify application messages.

The bounded decoder supports Graphlab's single-section, single-interface, little-endian PCAPNG profile: SHB, Ethernet IDB, EPB and ISB, microsecond timestamp resolution, no timestamp offset. Unsupported block layouts, inconsistent lengths, extra interfaces/sections, unsupported timestamp options and malformed block trailers reject the whole segment. No partial prefix is published as a successful index.

Ethernet, up to two VLAN headers, IPv4 and direct IPv6 TCP/UDP headers are decoded. ARP and ICMP are classified. Short/malformed headers, unknown EtherTypes, IPv4 fragments and IPv6 extension chains are explicit states. Fragments are not reassembled; IPv6 extension chains and jumbograms are not decoded. No payload, TCP stream, DNS content, checksums, message IDs, trace IDs or application correlation is inferred. Header `complete` means the supported headers were available, not that the packet was valid or delivered.

## Finalization, integrity and scheduling

Queries request a scan of that run's registered capture descriptors. Only segments in their atomically published manifests with `state: closed` and canonical sequence filenames are eligible. The manifest must match the registered run, capture, boot and mapping. The indexer opens the canonical file with `O_NOFOLLOW`, requires a regular file of the manifest size, and verifies the full SHA-256 before decoding. Manifest and decoded packet counts must agree. A record references those verified bytes; later queries read the derived index, not a fresh rehash of the source. Existing download verification remains independent and unchanged.

Active `.partial` files, unmanifested files and terminal recordings are excluded. Finalized rotations can be indexed while capture continues, without stopping or modifying the writer. Queries for runs without captures return empty results. Destroyed runs retain their index and original artifact references according to their separate retention policies. Capture coverage, including incomplete coverage, is reported independently; an indexed packet does not repair a capture gap.

A dedicated worker performs file reading, hashing and decoding outside the executor/traffic-lease worker. There is one active scan and at most one queued run snapshot; requests while occupied do not create an unbounded queue. The live view retries every five seconds. `indexing`, `indexBusy`, `indexRequestAccepted` and `indexError` expose scheduling and failures. Indexing is on demand, not an always-on background replay of every retained capture.

## Bounds, persistence and recovery

The private agent directory contains a separate `packet-history.sqlite`. Each segment's records and status commit atomically. A crash before commit leaves no accepted segment marker and a later scan retries; a committed marker prevents duplication after restart. Failed immutable segments also retain their failure status and are not silently retried; an explicit run rebuild retries them. Startup failure of this optional database makes packet queries unavailable, without preventing normal execution/capture.

Fixed limits for this increment:

| Resource | Bound / behavior |
|---|---|
| Input segment | 128 MiB; larger segments have a visible failure status. One file is buffered at a time. |
| Decoder | At most 1,000,000 blocks, each at most 1 MiB; packet snapshot at most 65,535 bytes. |
| Packet rows per segment | First 2,000 packets in file order; the whole supported file is still checked and counted. `record-limit` status reports exact omitted packet count. |
| Encoded packet row | At most 2,048 bytes. |
| Retained rows | At most 20,000 globally and 16 MiB of encoded packet bodies; oldest index IDs are removed to satisfy both ceilings. Byte pruning removes batches, so fewer records may remain. |
| Time retention | 24 hours from indexing, using agent wall time. Expired rows are pruned on ingestion/query. A wall-clock step can affect retention; this clock is never used as a latency measure. |
| SQLite main file | 64 MiB / 16,384 4-KiB pages. DELETE journaling, FULL synchronization and a 2-MiB page-cache target; rollback journal may temporarily approach another main-file size plus bookkeeping. This is not a filesystem quota for all agent data. |
| Segment status catalog | 1,000 recent keyed receipts globally, automatically recycled. Migration can temporarily retain up to 1,000 additional legacy receipts until their registered manifests supply ordering keys. A scan considers the newest 1,000 finalized segments of its run and automatically indexes at most 128 new segments per request. |
| Queries | 1–200 rows, default 100; console pages contain 50. |

Recent segment receipts survive packet-row expiry. When the catalog exceeds 1,000 keyed receipts, its oldest entries are evicted and a durable retirement boundary advances in the **same transaction** as the new segment's rows and receipt. Ordering is lexicographic `closedAt/runId/captureId/paddedSequence`, using the writer's UTC second-resolution closure timestamp. The boundary is monotonic even when wall clocks move backward. Automatic indexing excludes keys at or below it, preventing evicted captures from replaying after restart.

This is a bounded, lossy admission policy, not proof that every older segment was indexed: late manifests, timestamps that move backward, and newly discovered ties ordered below the boundary can be excluded too. Responses expose `retirementBoundary`, cumulative `retiredCatalogEntries`, `retiredSegmentsInLastScan`, `scanWindowTruncated`, and `legacySegmentCount`. The console explains exclusions and offers explicit replay. The newest-1,000 candidate window is bounded independently of the catalog; older source artifacts remain downloadable. Existing legacy receipts acquire keys from identity-checked registered manifests without replaying their packets. Unvisited legacy receipts stay pinned in their bounded migration allowance.

Initial indexed/omitted counts are not current retained-row counts. Responses expose global retained records/encoded bytes and retention generation. Capacity/format/checksum failures remain visible; original captures are unchanged. A database error rolls back the entire segment. `packet_storage_full` identifies SQLite capacity/filesystem exhaustion; no successful receipt is published for a rolled-back segment, allowing retry after space is restored.

## Authenticated API and pagination

- `GET /api/v1/runs/{id}/packet-history` returns the current page and schedules a bounded index scan.
- `POST /api/v1/runs/{id}/packet-history/query` accepts `node`, `edge`, `protocol`, `limit` and `cursor`. Existing session, Origin and CSRF rules apply. No filesystem path is accepted.
- CLI/RPC method `packet-history` uses `runId` plus the same optional fields and existing peer authentication.
- `POST /api/v1/runs/{id}/packet-history/rebuild` (body `{}`), RPC `packet-history.rebuild`, replaces that run's derived rows/receipts and queues verified replay of its newest 1,000 finalized segments, bypassing the retirement boundary for this explicit replay. Existing global row/byte quotas still apply and may prune older rows of other runs.
- `POST /api/v1/runs/{id}/packet-history/recover` (body `{"scope":"all-runs"}`), RPC `packet-history.recover`, replaces the entire derived packet database. Every registered run must be stopped or destroyed. The scope is deliberately explicit; this operation is not limited to the route's run. No capture files or execution/application-telemetry state are reset.

Maintenance routes require the same authenticated operator, Origin and CSRF checks as existing mutations. GET does not invoke maintenance. Unknown fields and unknown runs are rejected. Rebuild returns 409 `packet_index_busy` while a scan is queued/running; retry once the worker is idle. The maintenance snapshot contains an operation ID, run ID, and `queued`, `running`, `completed`, `failed` or `interrupted` state. Completion means the bounded scan finished; per-segment failures and omissions remain authoritative. The snapshot is a single bounded last-operation record, not an unbounded job log.

Responses use `apiVersion: graphlab.packet-history/v1`, with `items`, `segments`, `nextCursor`, capture coverage, indexing status and explicit limits. Node filters mean captures on incident declared edges, not claims that the node sent/received each packet. Filters intersect. Protocol values emitted by the decoder are `tcp`, `udp`, `arp`, `icmp`, `icmpv6`, `ipv4`, `ipv6`, and `other`.

Rows are ordered by descending index ID. The cursor encodes the first page's upper ID, next exclusive ID, run/filter scope, durable database epoch and retention generation. New indexing cannot enter an older-page snapshot. Cursors remain valid across agent restart, but changing run/filters or replacing the database is rejected. Any pruning invalidates outstanding cursors with HTTP 409 `packet_history_expired`; clients must explicitly return to newest. This conservatively invalidates cursors even when another run caused pruning. It does not pin rows against retention. Cursor values are navigation state, not authority; every query still binds its authenticated route's run and filters.

The console freezes an older page, resets pagination when selection/filters change, and offers return-to-newest. Each observation links to its exact artifact in the existing capture catalog; the existing download assembles and verifies the full SHA-256. Indexed timestamps, offsets, lengths, header states and raw metadata remain inspectable.

## Recovery procedure and failure behavior

Use **Packet index maintenance → Rebuild this run’s packet index** for a targeted replay, including retired or previously failed artifacts. Clearing the run's rows/receipts, invalidating cursors and recording the queued rebuild commit atomically. Packet ingestion remains transactional per segment. A crash during rebuild can leave a partial derived index; startup marks an unfinished operation `interrupted`. Reissue rebuild to clear the partial run index and replay it. Normal query scans remain bounded and do not silently perform a full administrative rebuild. Capture SHA-256 checks are repeated during replay.

For an unavailable/corrupt database, quiesce or destroy all runs, then use **Recover packet index for all runs** and select its explicit all-runs acknowledgement. The agent stops the index worker, durably writes `packet-history.sqlite.recovering`, moves the database and any SQLite sidecars to `.quarantine` names, builds a fresh database at `.replacement`, publishes it, syncs the directory and clears the intent marker. A new database epoch invalidates all old cursors. Subsequent queries reindex retained sources within the ordinary candidate/admission limits.

If recovery is interrupted, the intent marker prevents automatic opening of the partially replaced index; reissue recovery to finish idempotently. One quarantine set is preserved, never overwritten by a later independent recovery. A second recovery returns `packet_quarantine_exists`; an administrator must first archive that set outside the active state directory while the index is idle. Quarantine and replacement files require additional space beyond the 64-MiB main-file budget. Recovery cannot manufacture free disk space; restore capacity before retrying a storage failure. Corrupt indexes are never automatically deleted just because a query fails.

Crash and page-limit hooks exist only in builds configured with `GRAPHLAB_TEST_CHECKPOINTS=ON`, which must not be deployed. Qualification kills processes before/after segment commit, before/after rebuild clearing, and after quarantine/publish during recovery. Tests verify SQLite hot-journal rollback, duplicate-free retry, cursor invalidation and interrupted-operation status. A separate dedicated 8-MiB tmpfs test reaches real `ENOSPC`, verifies atomic rollback, frees space and verifies one successful retry. This tests process interruption and filesystem exhaustion, not power-loss durability or every possible filesystem error.

## Reference and validation

Reference inspection used graphx-docker commit `7cad4da8646eda302a005070228495c1aa87d89a`: `apps/telemetry/history.mjs` (bounded worker queue, retention/configuration and filter contract), `history-worker.mjs` (SQLite limits and descending-ID pagination), `http-routes.mjs` (packet-history proxy accepts `limit` up to 500, `before`, and TCP/UDP `protocol`), `metric-store.mjs` (`network_packet` and bounded recent observations), `capture-files.mjs` (PCAPNG structure checks), and `web/src/components/HistoryPanel.jsx` (frozen older pages, packet metadata and return-to-newest). Graphlab does not copy the reference's inference that a network packet updates both sent and received application counters. Its index derives only from its own capture artifacts, with its own scope-bound cursor and smaller query ceiling.

See [the parity report](web-console-feature-parity.md#finalized-packet-history-increment--23-september-2026) for portable, browser and dedicated Linux evidence. Active-file indexing, arbitrary imported PCAP formats, payload inspection, reassembly, application-message correlation and packet direction attribution remain outside this increment. Automatic catalog recycling and explicit rebuild/recovery are now implemented; quarantine archival remains an administrator action.
