# M6 operational qualification

M6 adds an executable qualification workflow, rather than treating earlier milestone test summaries as a release certification. The [runner and fixture instructions](../qualification/README.md) describe the real Linux checks. The [acceptance matrix](../qualification/matrix.json) keeps all T01–T19 gates explicit; a partial, blocked or failed required gate prevents qualification.

The [remediation report](validation/m6/remediation.md) tracks subsequent fixes and additional qualification fixtures separately from the immutable initial evidence bundle.

The [completion report](validation/m6/completion.md) records the staged closure of the eight remaining gates, including live services/PTYs, 45 crash cases, the one-hour capacity matrix and independent protocol-minor releases. Its separate bundle retains the original evidence and identifies which historical checks were reused. Qualification is scoped to the selected Linux ARM64 runtime and declared capacity envelope.

## Implemented tooling

- `lab-qualify`: C++23 streaming SHA-256 hashing, evidence manifest creation and strict integrity/coverage verification. Required runtime, source, workload and capacity artifacts must be retained and hash-bound; gate evidence must refer to those files. Exit 1 means an intact but incomplete qualification; exit 2 means invalid or changed evidence. The verifier does not manufacture experimental proof from arbitrary operator-supplied claims.
- `qualification/run-linux.sh`: reproducible, explicit Linux invocation of the CTest suite and privileged M2–M5 acceptance fixtures, followed by capacity measurements, runtime inventory and cleanup observations. Every fixture's exit code is retained.
- `m6_capacity`: a C++ numbered UDP echo generator bound to `data0`, comparing baseline, telemetry-only and all-edge-capture profiles. It records latency, throughput, delivered loss separately from raw capture drops, CPU, memory, I/O and capture storage. Direct-edge and redundant-triangle modes use the same arbitrary graph executor.
- `m6_reboot`: prepare/verify stages for an active capture and recorded-console run across an explicit host reboot. It checks boot/generation discontinuity and rejects old capture/terminal authority before scoped cleanup.
- Portable negative tests for missing/duplicate gate identities, partial qualification, altered artifacts and escaping evidence paths. The ordinary CTest suite exercises these without root or Docker.
- Runner failure/skip propagation and capacity unknown-value regression tests, plus bounded recording replay and a seeded malformed-recording corpus. A browser corpus exercises hostile terminal escapes without HTML or clipboard integration.
- Linux `m6_network` and `m6_direction` fixtures for tagged trunks, bounded RSTP recovery and numbered endpoint observations under asymmetric impairment and offload variants.

## Observation profiles

Run admission accepts `observationProfile: "full"` (default) or `"minimal"`. The selection is journaled with the run and cannot be changed in place. `minimal` disables periodic interface/qdisc/tree collection and its five-second resource-health observations. It **does not disable** fault expiry/recovery, capture health/lease renewal or guest watchdog enforcement. The telemetry response exposes the chosen profile; missing observations are not represented as zero traffic.

For example, include this field in the existing authenticated `POST /api/v1/runs` body or `lab control ... start PARAMS.json` request:

```json
{"topologyHash":"sha256:YOUR_VALIDATED_HASH","idempotencyKey":"minimal-profile-001","developmentMode":true,"observationProfile":"minimal"}
```

The baseline benchmark uses minimal observations with an explicitly non-capture development run. The other profiles use normal telemetry, with capture added only for the third comparison. Minimal mode still keeps the serialized executor and journal; baseline does not mean zero controller overhead.

## Evidence and retained artifacts

[Measured results and qualification disposition](validation/m6/README.md) identify the selected runtime, actual checks and outstanding gates. Large independent image, package and source archives reside in ignored `qualification/artifacts/linux-arm64-m6/`; copy that directory when preserving or transferring this workspace. Git contains the reviewed manifest, runtime evidence, measurements and reports, not the image layers themselves. The PowerPC/TCG and ARM64/KVM example artifacts remain in their existing `qemu-guests/` locations.

A runtime lock here records exact observed tool/package versions, binary/image identities and source bytes for compatibility evidence. It is not a deployment service that automatically installs or enforces those versions. Do not use the M0 historical `locks/runtime.json` as an M6 certification.
