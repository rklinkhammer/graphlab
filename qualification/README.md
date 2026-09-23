# Operational qualification

M6 supplies reproducible Linux fixtures, packet/capture capacity measurements, retained workload artifacts and a fail-closed evidence manifest. It does not turn partial coverage into a release pass. The current T01–T19 matrix is `matrix.json`; each status is an explicit review of named evidence, not a substring search for “PASS”.

Build `lab-qualify`, `m6_capacity` and `m6_reboot` with the normal C++23 project. The latter two are Linux-only, root, explicit-invocation tools. No Python is required. Run the integration runner on a dedicated Docker/OVS/systemd host, with trusted root-owned 0755 capture/terminal workers, independently built immutable workload images, and the two preserved guest artifact directories:

```sh
qualification/run-linux.sh "$PWD" /absolute/new/evidence \
  sha256:APP_A_IMAGE_ID sha256:APP_B_IMAGE_ID \
  /absolute/ppc64le-artifacts /absolute/arm64-artifacts
```

The runner executes CTest, privileged transport, M2–M5 kernel/runtime fixtures, tagged-trunk/failover and offload/direction checks, and capacity tests. `exits.txt` records every exit code; after collecting evidence, the runner exits nonzero if any fixture failed or returned a skip code. Review the log before constructing a release manifest. No other lab agent/fixture may hold `/run/graphlab-executor.lock`. Preserve interrupted fixture paths for scoped recovery rather than deleting network objects globally.

`m6_capacity SOURCE IMAGE_ID NEW_OUTPUT SECONDS` uses data-interface-bound numbered 256-byte UDP echo requests at 250/1,000/4,000 requests/s, twice per profile. Profiles are minimal telemetry/no capture, full telemetry/no capture, and full telemetry/all-edge capture. Capture barriers and leases remain active in the capture profile. The default topology is a direct Docker edge; `GRAPHLAB_CAPACITY_TRIANGLE=1` selects a three-switch redundant topology. Sample duration is bounded to 5–300 seconds; the runner defaults to ten. It reports per-sample p50/p95/p99 RTT, sent/received/loss, achieved rate, VM CPU busy percentage, host memory available, controller RSS, host paging I/O, recording growth and raw capture statistics. A sample meets its declared envelope only if the run stays ready, delivery loss is at most 1%, and achieved rate is at least 95% of target. This is a measured sample envelope, not a long-term capacity guarantee. CPU/I/O include background VM activity; memory is not aggregate per-container/capture RSS. Offloads are not changed. Capture counters are cumulative source counters, not inferred delivered packet loss.

`m6_reboot prepare SOURCE IMAGE_ID NEW_STATE_ROOT` creates a capture-enabled run and recorded Docker console. Reboot the dedicated host, then invoke `m6_reboot verify SOURCE IMAGE_ID SAME_STATE_ROOT`. The second stage rejects an unchanged boot ID, verifies generation/authority discontinuity and retained artifacts, then performs scoped cleanup. Do not reboot a shared host without checking its other workloads.

After reviewing the evidence, copy logs, runtime lock, source snapshot, independent workload archive and capacity report into one private directory; fill `matrix.json` with actual gate dispositions. Then:

```sh
build/dev/lab-qualify record /absolute/evidence qualification/matrix.json
build/dev/lab-qualify verify /absolute/evidence/manifest.json
```

The recorder hashes the directory's top-level files and refuses to overwrite a manifest. The verifier requires exactly T01–T19, required artifact references, evidence for passed gates, matching SHA-256 bytes, and paths confined to the evidence directory. Exit 0 means every gate is recorded as passed; exit 1 means integrity passes but required gates are incomplete; exit 2 means invalid/tampered evidence. This is an integrity/coverage checker for operator-reviewed evidence, not an independent proof that arbitrary supplied logs demonstrate each claim or a signed attestation.

Large image/source/package archives stay in ignored `qualification/artifacts/`; summaries and measured evidence belong in `docs/validation/m6/`. A manifest only verifies when its corresponding archives are present. Preserve those files when moving the workspace; they are intentionally not committed as Git blobs.

Use `lab-qualify summarize CAPACITY.json` to generate the measured profile table with CPU percentage-point and RTT deltas relative to baseline. Its RTT column is the mean of per-sample p95s, not a pooled percentile. Keep the raw JSON with the table. Use a short root-owned parent (for example `/tmp/gl6-run`) for benchmark state: worker capability restrictions intentionally do not bypass arbitrary user-private ancestors, and Unix socket paths are bounded.

Unknown RTT or capture counters remain `unknown`; missing workers and observation gaps make the capture-observation column incomplete. A delivery pass is independent of recording completeness. `m6_network SOURCE IMAGE_ID` checks a real tagged trunk and a predeclared 15-second delivery-restoration bound after a forwarding-link cut. `m6_direction SOURCE IMAGE_ID` observes numbered packets at both Docker endpoints with GRO/GSO enabled and disabled, measuring asymmetric delay and loss independently of capture counters.

Set `GRAPHLAB_CAPACITY_BUDGET_MIB` (256–6,144; default 256) explicitly when a larger graph needs more recording storage. Budgets are divided among edge workers, so hot edges may reach their share before the whole run uses the nominal budget. A quota-triggered quiescence is a storage limit, not proof of a packet-processing ceiling; retain that outcome and use a new output directory for any larger-budget follow-up.

## Completing the remaining M6 gates

The staged procedure and retained failures/results are documented in [completion evidence](../docs/validation/m6/completion.md). Original and remediation bundles remain immutable; the extension lives in `artifacts/linux-arm64-m6-completion/`.

- T04: `sudo build/dev/m6_barrier SOURCE IMAGE_ID` covers first numbered frames and failed-arm holds on all named shapes.
- T07/T09: `live-services.sh` plus `console/web/tests/m6-services-live.spec.ts` exercise real browser/CLI faults and independent service restarts.
- T10/T17: configure with `-DGRAPHLAB_TEST_CHECKPOINTS=ON`, then run `m6_crash SOURCE IMAGE_ID PPC_INPUT ARM_INPUT` and its `--audit EVIDENCE_ROOT` phase as root. Checkpoints kill the executor at named boundaries. Reconfigure OFF before normal deployment/measurements.
- For retained failed workers, `sudo build/dev/m6_cleanup EXPLICIT_FIXTURE_ROOT...` verifies root-owned descriptors and inactive/failed state, invokes normal production cleanup, and requires unit/socket absence. It refuses active workers. Never substitute a global `reset-failed` or resource deletion for scoped recovery.
- T12: `sudo build/dev/m6_terminal_live SOURCE IMAGE_ID`, and the live WebSocket fixture `m6-terminal-live.spec.ts` with the service fixture. The storage policy retains a bounded prefix and explicitly stops recording at quota; it does not silently evict earlier bytes.
- T18: `sudo sh qualification/sustain-linux.sh SOURCE IMAGE_ID /tmp/gl6-NEW` performs the one-hour [declared envelope](remaining-gates.md). It requires two 300-second samples per profile, fixed 6 GiB recording budget, and stricter loss/rate/resource/continuity limits than the exploratory sweep above. Do not overlap builds or other fixtures.
- T19: the [minor matrix](protocol-minor-matrix.md) requires a frozen pre-change controller fixture and 1.0 sources. `build-protocol-releases.sh SOURCE FROZEN_SOURCE_1_0 NEW_OUTPUT IMMUTABLE_BASE_ID` creates complete installed SDKs and four independent app images. Run the frozen and current `m6_protocol SOURCE RELEASES_JSON legacy|current` binaries as root, serially. The current fixture also tests an image that falsely advertises v1 while returning wire major 2.

The SDK archive must include `include`, `lib` **and** `share`; omitting transitive CMake metadata prevents clean consumers from configuring. Set `GRAPHLAB_DEPENDENCY_PREFIX` to the installed/extracted development dependency prefix if it differs from the selected VM. Nodes select their exact SDK with `-DLAB_SUPPORT_VERSION=1.0.0` or `1.1.0`. These versions are local qualification releases; no registry or remote repository publication occurs.
