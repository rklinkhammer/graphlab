# M3: capture-first execution

M3 adds C++23 libpcap workers, a PCAPNG writer, capture barriers, renewable node traffic leases, and artifact access to the M2 executor. Docker fixtures consume the common C++ lifecycle library; no Python support was added. Required-capture runs use the same single agent and SQLite journal as development runs.

## Build and launch

Keep the [M2 prerequisites and account separation](m2-executor.md). Linux additionally requires libpcap development headers/libraries and systemd. The tested libpcap version is 1.10.4. CMake builds `lab-capture` on Linux and the portable writer/contract tests on macOS. Install `lab-capture` beside `lab-agent`; the worker executable must be a regular root-owned file without group/other write permissions. Install the whole binary directory in a trusted root-owned location for persistent use.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
sudo cmake --install build/dev --prefix /usr/local
npm ci --prefix console/web
npm run build --prefix console/web
```

Rebuild app-a and app-b independently against the installed common package as documented in M2, then build new immutable images. M3 images advertise `graphlab.gate-protocol=1` and `graphlab.traffic-lease=1`. Required-capture admission rejects older images without lease support. Labels are declarations by trusted workload publishers, not image attestation. Existing synthetic workload/package metadata in the fixture generator remain test metadata, not a published supply chain.

Agent/API launch flags are unchanged. A topology with `capture.required: true` opts into M3; `capture.required: false` still requires explicit `developmentMode: true`. QEMU execution is now provided by [M4](m4-qemu-consoles.md); the M3 qualification below remains Docker-specific. The CLI uses `lab control --socket ... --agent-uid 0 start start.json`:

```json
{
  "topologyHash": "sha256:REPLACE_WITH_CATALOG_HASH",
  "idempotencyKey": "capture-run-0001",
  "capturePolicy": {
    "runBytes": 6442450944,
    "reserveBytes": 5368709120,
    "rotateBytes": 67108864,
    "rotateSeconds": 60
  }
}
```

The policy is optional; these are its defaults. The retained-data admission ceiling is 8 GiB per agent state root. Per-worker allocations sum to the run byte budget, with 8 MiB per edge additionally reserved for metadata and small-file allocation. Each worker is bounded to 1,024 segments, 64 MiB of systemd memory and four tasks. Admission accounts for workload memory plus 64 MiB per capture against 80% of currently available host memory. The minimum per-edge allocation is 256 KiB. The journal remains outside worker-writable directories, and the free-space reserve is checked while recording. Explicit test policies may lower the reserve to 1 MiB; production defaults reserve 5 GiB. This reservation accounts for Graphlab's storage, not unrelated host writers.

## Barrier and lease behavior

1. Check capacity before creating runtime resources. Prepare all declared edges with workload gates held.
2. Journal every capture descriptor before launch. Choose a verified host-side endpoint where one exists; direct Docker-to-Docker links use a verified container namespace. Record ordered endpoint/mapping information, namespace inode, ifindex, boot ID, capture epoch, worker ID and ownership nonce.
3. Enable only the selected capture endpoint so Linux libpcap can activate; its peer and application gate remain held. This endpoint-enable interval is explicitly outside the capture coverage promise.
4. Start an independent transient systemd service for each data edge. The worker verifies namespace/interface identity, activates libpcap, installs the filter and writes/syncs PCAPNG headers. A controller acknowledgement is serviced by the packet-draining loop, not inferred from a PID, file or arbitrary delay.
5. Once every required worker acknowledges readiness, commit the armed barrier, activate links and converge RSTP, recheck workers, then release the node gates with ten-second leases.

The agent checks workers and renews leases approximately every two seconds while the run is ready. Capture failure records incomplete coverage and requests quiescence. If the agent is absent, the common node library expires its lease locally. It uses nonblocking UDP sockets, a monotonic deadline and bounded polling. Expired leases cannot be renewed into an implicit release; a fresh barrier is required. The live qualification tests measure the fixture's quiescence against a 12-second target, including polling and control overhead. This is a measured lab profile, not a real-time scheduling guarantee for arbitrary hosts or third-party workloads.

Quiesce holds the workload gates before finalizing captures. Resume starts a new capture epoch and repeats the barrier. Destroy/recover closes or accounts for workers before deleting their interfaces. Previously incomplete coverage remains `closed-incomplete`; closing files does not erase a gap.

An isolated graph has zero required captures. No fictitious stream is created. The tested backend handles switch-switch, Docker-switch and direct Docker-Docker edges. Capture count comes from the declared data edges, not a fixed graph shape.

## Supervision and restart

Workers are systemd services named `graphlab-cap-<id>.service`, without stop/restart dependencies on the API or agent. They do not restart themselves or acquire new topology resources. Initial capabilities are bounded to network capture and namespace entry; after activation, workers drop all capabilities. Their filesystem is read-only except for the allocated capture directory, and `NoNewPrivileges` is enabled.

The root-only Unix control protocol requires the registered nonce and controller generation. Adoption checks boot, run, mapping and systemd invocation identities. The worker advances its controller generation durably; stale requests are rejected. Worker heartbeats/manifests use a monotonic source sequence within the worker invocation, so reading a snapshot again does not manufacture a new capture event.

After agent restart, surviving captures continue recording. The agent adopts them and quiesces the run into `reconciling`; it never revives a stale traffic lease automatically. The current recovery operation cleans up that run; a new run establishes a fresh experiment. Already stopped runs retain their closed capture state without adopting exited workers. Known incomplete coverage remains incomplete. API restart affects client sessions but not workers. A prior-boot worker identity cannot be adopted. Reboot handling is coded defensively but has not been qualified by rebooting the shared lab VM.

Required-capture admission requires reversible quiescence (`supported`) in every Docker workload contract. Contracts advertising `restart-required` are rejected before run admission; automatic workload recreation is not implemented in this milestone.

## PCAPNG and artifacts

The writer implements little-endian Section Header, Interface Description, Enhanced Packet and Interface Statistics blocks, using microsecond timestamps and four-byte alignment. It pins the layout described by [PCAPNG draft 05](https://www.ietf.org/archive/id/draft-ietf-opsawg-pcapng-05.html), a work-in-progress format reference, not a finalized RFC. Independent libpcap readback tests check generated files and packet timestamps/lengths.

Active files end in `.partial`. Rotation/finalization syncs the file, publishes a closed filename without replacing any existing segment, syncs the directory and records a SHA-256/size manifest. The normal flush interval is one second. Requested rotation and final close include a bounded drain window; this does not promise loss-free kernel delivery. Segments are never overwritten or ring-evicted. Structural inspection reports the valid prefix of interrupted files without rewriting their original bytes. Partial files are visible but are not presented as completed downloads.

Statistics distinguish cumulative libpcap receive/drop counters from per-segment written packet counts. Unavailable counters remain null. Offload state and individual packet direction are currently unknown and explicitly described as such; the selected endpoint's RX/TX interpretation is recorded, but no per-packet direction guarantee is inferred. Capture readiness does not guarantee zero drops, capture before interface creation, or delivery after downstream queues.

Authenticated routes:

- `GET /api/v1/runs/{runId}/artifacts`: closed segment metadata and partial-stream entries.
- `GET /api/v1/runs/{runId}/artifacts/{artifactId}/chunks/{offset}`: bounded base64 chunks of a closed artifact; offsets are decimal strings.
- CLI RPC methods `artifacts` (`runId`) and `artifact` (`runId`, `id`, `offset`) use the same authority.

Clients supply artifact IDs, never filesystem paths. Existing session, Origin and peer checks apply. The browser reconstructs a closed file and verifies its SHA-256 before downloading it. Artifacts remain accessible for destroyed runs. There is no automatic retention deletion, pinning UI, live partial export, or additional ad-hoc capture endpoint in this milestone.

## Verification

See [M3 verification](validation/m3-verification.md). The explicit root/Linux suite is:

```sh
sudo build/dev/m3_linux "$PWD" "$PWD/build/dev/m2_tests" \
  sha256:APP_A_IMAGE_ID sha256:APP_B_IMAGE_ID
```

It creates only test-owned resources. Standard CTest covers writer, journal, barrier and protocol behavior without touching Docker or host networking. The separate browser test requires `GRAPHLAB_M3_LIVE=1` and the dedicated Linux fixture/tunnel on port 18089. Skipped live tests are not passed acceptance gates.
