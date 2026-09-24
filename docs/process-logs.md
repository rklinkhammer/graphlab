# Retained process-log snapshots v1

Increment C extends the existing runtime-tail viewer with durable, immutable snapshots. It does not provide a continuous log or a serial recording. The reference `graphx-docker` revision recorded in the parity report supplies bounded node-console snapshots; durable retention is a Graphlab extension.

## Sources and contract

`graphlab.process-log-snapshot/v1` binds each artifact to `runId`, `node`, `sourceId`, `generation`, a decimal `id`, `collectorEpoch`, observation/finalization times, decimal byte `size`, and `sha256`. Downloads carry base64 of the collected bytes. Docker output combines stdout/stderr returned by `docker logs --timestamps --tail 200 --since StartedAt`; generation is immutable container ID plus StartedAt. Labels are checked against the run/resource generation. Docker may already have normalized invalid bytes. Graphlab preserves the returned bytes, not necessarily original writes.

`qemu-worker` reads the exact owned worker unit with its verified systemd invocation ID using journalctl short-iso output. This is the worker journal, not guest serial or the separate QEMU stderr file. `qemu-runner`, when present, reads the independently verified runner container's stdout/stderr with the Docker rules above. Empty output is valid. No second serial reader or capture process is introduced.

Times describe agent observation/finalization or runtime-rendered log timestamps; clocks are wall clocks and are not assumed synchronized. Ordering uses SQLite's monotonic artifact sequence, not event timestamps. Source generation changes and agent restarts publish explicit boundaries. Equal bytes are coalesced only in the same generation and collector epoch while the artifact remains retained. Changed snapshots may overlap, and bytes between observations may be missing. `gap`, `coverage`, `omissionPossible`, `lineLimit`, `limitBytes`, and `truncated` expose this boundary. Byte truncation keeps the newest 65,536 bytes and can split a character or line. There is no exact omitted-byte count.

## Collection and storage

One collector thread polls existing sources, independently of execution/capture/terminal recording. Each sweep admits up to 16 round-robin source descriptors, at most 32 KiB each, with one runtime read in flight. Runtime reads time out after two seconds, cap command output at 1 MiB and retain at most 200 lines/64 KiB. Sweeps wait five seconds; this is best effort, not a sampling guarantee. Slow sources delay other log sources. Descriptor and collection failures are visible diagnostics. No log-consumer queue can grow without bound.

`process-logs.sqlite` uses mode 0600, SQLite FULL synchronous transactions and DELETE journaling. Payload and metadata publish atomically. Limits are 128 artifacts/4 MiB per source, 512 artifacts/32 MiB globally, and 24 hours. Oldest artifacts are evicted; source ledgers retain eviction counts. The source catalog is capped at 256 identities for the state-directory lifetime, including retired sources. Further sources are rejected visibly; no archival/reset UI is supplied. Metadata input is capped at 2 KiB. Database pages are capped at 16,384 (normally 64 MiB); rollback journals and SQLite overhead are additional, so logical payload quotas are not a filesystem reservation.

Failed transactions roll back and preserve committed artifacts. Disk-full errors are surfaced; recovery retries later. Restart reuses committed artifacts and marks prior collector observations stale. Destruction leaves retained artifacts accessible until retention or quota eviction. Collection cannot recover runtime output already discarded before attachment or between polls. Store initialization failures disable retained access without changing run execution. A separate directory/filesystem quota remains an operator concern.

## API and presentation

Authenticated, Origin/CSRF-protected POST endpoints:

- `/api/v1/runs/{run}/process-logs/query`: `{node, source?, cursor?}`; 20 artifacts newest first, source availability/errors/evictions and a scope-bound exclusive sequence cursor.
- `/api/v1/runs/{run}/process-logs/download`: `{node,id}`; exact retained artifact with size and SHA-256 verified before release.

Run and topology-node membership are checked before lookup; neither endpoint accepts filesystem paths, container IDs or journal units. Cursors remain stable as new artifacts arrive, but retention may remove old rows. Missing/expired/wrong-scope downloads fail closed. Integrity failures return an error instead of bytes.

Catalog-refresh failures and artifact-action failures have separate alerts. Background polling clears only catalog errors; artifact errors persist until a new action or explicit navigation changes the selection scope.

The Logs panel separates live runtime tails from retained artifacts. Newest catalogs refresh; older pages freeze until navigation. Selection is fenced across source/page/run/node changes. Search applies only to the selected snapshot. React renders output as text with invalid UTF-8 replacement for display; downloads preserve the collected bytes.

## Qualification boundary

Native tests cover deduplication, source generations, arbitrary bytes, bounds, stable pagination, wrong scope, transactional rollback, checksum rejection, expiry and restart. Dedicated Linux tests exercise real Docker output/tail rollover, authenticated API/browser downloads, agent restart and destruction; QEMU worker and runner artifacts are compared with independent runtime reads. The runner fixture emits no output, so that comparison verifies an empty artifact. An isolated 8 MiB tmpfs verifies actual ENOSPC rollback and retry.

This is bounded snapshot retention, not lossless streaming. Docker log-driver file rotation itself and power-loss recovery are not newly qualified; tail-window rollover, generation boundaries and database restart are. QEMU guest output and application-message correlation remain separate features.
