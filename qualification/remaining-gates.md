# Remaining M6 qualification: declared criteria

These criteria precede the new measurements. They supplement, and do not replace, T01–T19 in `docs/option-d-console-plan.md`. Historical bundles remain immutable. A gate changes to passed only after its fixture and cleanup checks pass on the selected ARM64 Linux runtime.

## T18 operating envelope

The candidate supported envelope is two Docker endpoints, either one direct edge or the five-edge/three-switch RSTP triangle, with 256-byte numbered UDP requests at 1,000 requests/s and their replies. The triangle uses fixed root priorities. Baseline, telemetry-only and all-edge capture each run for two consecutive 300-second samples per graph (ten minutes per profile, one hour total). Earlier 250/1,000/4,000 rate sweeps remain supporting evidence; 4,000 requests/s is a stress observation, not the supported sustained limit.

Pass thresholds: delivered loss <=0.1%, achieved rate >=99% of target, zero capture source drop increments, ready state throughout, no worker restarts or recording gaps, p95 RTT <=20 ms, mean whole-host CPU <=25% on the eight-vCPU VM, >=2 GiB available host memory, and controller RSS growth <=64 MiB within a profile. Publish all CPU, memory, I/O, latency, throughput and recording rates, including failures. Recording budget is 6 GiB per capture run; no retention overwrite is permitted. Exceeding a limit fails the candidate envelope; do not widen limits after seeing results. These bounds qualify this measured runtime, graph size, load and duration only.

## T19 compatibility matrix

Freeze and retain the current installed shared package/node sources as protocol 1.0 before introducing the additive 1.1 implementation. Protocol 1.1 must preserve the 1.0 control commands and response fields; its additions are explicitly optional. Exercise independently built app-a and app-b against both package versions, and mixed 1.0/1.1 deployments in both directions. Include legacy/current controller interpretation and unsupported major 2 rejection before release. Archive both versioned package/source artifacts and resulting image identities; no sibling source checkout or Python runtime may be required. These are locally retained qualification versions, not claims of externally published releases.

## Stage order

1. T04: first numbered frame at every declared capture point on all named graph shapes, with activation-before-release proof and failed-arm holds. T07: independent API and agent restart during capture and terminal output, including duplicate import and failure discontinuity. T09: real browser/CLI simultaneous fault requests, same-key replay, conflicting payloads and stale revisions.
2. T10/T17: inject interruption around intent, create, readback and completion for QEMU, terminal and fault operations, including teardown cancellation; recover twice and inspect resource types while preserving unrelated sentinels.
3. T12: live slow viewers, reconnect/resize, independent cursors, default/opt-in input recording, storage limits and explicit recording gaps. Retention behavior must match the implementation's declared policy.
4. T18: run the declared sustained envelope above.
5. T19: run the frozen-version compatibility matrix above.
