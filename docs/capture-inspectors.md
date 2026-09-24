# Evidence-backed inspectors and capture metadata

Increment D adds readable provenance and availability to existing observations. It does not introduce a diagnostic inference engine or a new capture process.

## Application and network views

Application transport, framing, schema and network associations come from the validated topology, identified by its topology hash. Unspecified protocol fields remain unspecified. Associations describe declared resources, not observed routes or delivery. Multiple associated network edges retain individual navigation buttons; no first-edge selection or summed traffic total is inferred.

The resource inspector identifies the runtime snapshot time and shared failure domains supplied by inventory. The link-performance panel separately displays interface administrative/carrier/operational state, mapping/counter epochs, reported collection failure, and RSTP port observations. Interface evidence retains its observation time/source and stale status. RSTP has a separately cached host-monotonic observation time; it must not be compared with wall clocks or other hosts. Missing/empty RSTP remains unavailable or not applicable. Enabled interfaces and forwarding state are not proof of application delivery. Root cause and failure layer remain undetermined.

## Finalized capture metadata

Newly finalized segments retain these additional manifest fields:

| Field | Source / meaning |
|---|---|
| `format` | `pcapng`, emitted by the writer |
| `linkType` | `1` (Ethernet); matches the emitted IDB and the worker's DLT_EN10MB requirement |
| `interface`, `snaplen` | Writer configuration, also emitted in the IDB |
| `packetLengths.apiVersion` | `graphlab.capture-lengths/v1` |
| `packetLengths.capturedBytes` | Decimal sum of successfully written EPB captured lengths |
| `packetLengths.originalBytes` | Decimal sum of those EPBs' original lengths |
| `packetLengths.truncatedPackets` | Decimal count where captured length is less than original length |

Counters reset on each segment. They exclude PCAPNG block overhead, include only recorded packets, and do not quantify capture drops or delivery loss. Existing `packets`, `closedAt`, size, SHA-256 and libpcap statistics retain their meanings. `closedAt` is the worker's wall-clock finalization timestamp, not filesystem mtime or the last packet timestamp. These fields are published through the existing atomic manifest lifecycle, after segment finalization. No additional per-packet persistence or indexing is added.

The authenticated run catalog adds descriptor-backed `canonicalEndpoint`, `captureInterface`, `mappingEpoch`, `limits` (snaplen, per-capture byte budget and rotation thresholds), `runId`, and explicitly run-level `captureCoverage`. The declared capture node is the node portion of the canonical endpoint. Capture epoch and immutable mapping/run/boot identities are checked against the manifest before publishing finalized metadata or downloading the segment. Controller adoption generations may change independently; historical segments remain attached to their original capture epoch/mapping.

The catalog distinguishes source provenance from download verification. File bytes are not rehashed merely to list metadata; existing downloads verify their manifest checksum. Legacy segments without the new fields show unavailable values rather than zero or invented summaries. Active/unmanifested captures retain their existing incomplete state and do not claim finalized metadata. Terminal recordings retain their separate format and download path.

## Navigation and limits

An artifact can explicitly select its network edge. Application edges retain explicit endpoint/network navigation; packet observations retain exact capture artifact/block references. Node-scoped packet queries use the documented incident-edge union. No application-message correlation is added.

Verification includes native writer/rotation/identity tests, independent Linux libpcap reads, browser stale/unknown/provenance/checksum tests, and a real capture-first Linux run whose five finalized artifacts and 988 packet records matched independent manifest/PCAPNG parsing. See the [parity report](web-console-feature-parity.md) and `docs/validation/inspectors-*` evidence.

Legacy summaries are not backfilled. There is no imported-format support, per-packet direction attribution, synchronized-clock assumption, inferred route, delivery-loss measurement or definitive failure diagnosis. New live qualification used Docker traffic and host capture interfaces; it does not claim a new QEMU traffic qualification. Previously documented A–C limits remain.
