# M2: one durable executor

For current end-to-end startup, use **[Run the console for a specific topology](run-console.md)**. This document describes the historical milestone profile and its qualification; it is not the primary console quickstart.

M2 adds an opt-in Linux executor to the C++23 agent. SQLite persists runs, jobs, idempotency records and resource intents. One worker performs Docker Engine API and OVS/Linux networking operations; both HTTP and CLI are clients of that authority. The browser adds run/job controls. Independent C++ fixture nodes live under `docker-nodes/app-a` and `docker-nodes/app-b` and consume the installed `LabSupport::lifecycle` package.

This is the **development execution profile without capture coverage**. A start requires `developmentMode: true` and a topology with `capture.required: false`. Required-capture topologies fail admission until M3. QEMU nodes fail admission until M4. Existing M0/M1 example topologies are deliberately not executable as-is.

## Implemented behavior

- One host executor lock plus an exclusive per-state-directory lock. One active run and one pending mutation per agent. New starts reject existing Graphlab-tagged Docker networks/containers or OVS bridges, including resources from another state directory that require recovery.
- SQLite WAL, synchronous FULL, prepared statements and atomic journal commits. The bounded v1 journal stores a versioned JSON state document in one SQLite row (16 MiB, at most 256 retained jobs). A failed commit closes admission; it does not authorize unjournaled effects. This is not the future event/history database schema.
- Per-principal idempotency keys: identical retries return the original job; different payloads conflict. Mutations of an existing run require its current decimal-string revision. Concurrent operations conflict. Reads remain separate from the worker's long-running backend operations.
- Start, quiesce/stop, resume, destroy, recovery cleanup, job status and cooperative start cancellation. Resource intents are committed before creation and identities afterward. A failed or interrupted run is `reconciling`; recovery removes its verified resources rather than automatically resuming uncertain traffic.
- Installed-image digest and architecture checks, Docker API 1.52 minimum, declared CPU/memory budgets against host capacity, management/host route conflict checks and explicit C++ gate-protocol image labeling. No registry pull is performed.
- Labeled containers, one owned OVS bridge per logical switch, access/trunk VLAN configuration, RSTP convergence, MTU/address application, veth edges and separate internal Docker management networks. All declared graph edges are generated from the topology, not a hard-coded triangle.
- Startup-held C++ nodes. Gates release only after link configuration and convergence, and acknowledge readiness. The shared fixture library supports quiesce/resume and a UDP echo/probe bound to `data0`'s address. Isolated nodes can release without a data address. Node containers have no Docker socket, drop all capabilities, use a read-only root filesystem and a small private `/run` tmpfs, and have IP forwarding disabled. Host data bridges/veths have IPv6 disabled and no assigned host IPs.
- Teardown checks Docker IDs/labels, OVS UUIDs/ownership tags and link aliases/ifindexes. Creation-time deterministic locally administered MACs let recovery identify an interrupted veth creation before alias assignment. Existing names without matching ownership are conflicts. No global Docker/OVS/network flush is used.
- Absence is accepted only from successful OVS or link inventory queries. Lookup failures fail the job and retain uncertain resources for recovery. Resume and edge removal check the saved container/namespace identity, including a namespace precheck before changing either endpoint; the namespace descriptor stays open for the operation.
- Identity snapshots appear in the existing graph inspector. Observation time is explicit; these are not continuously refreshed kernel observations or telemetry. Capture coverage is always unavailable in M2.

## Build

Add SQLite development headers/libraries to the [M1 prerequisites](m1-console.md). CMake discovers the platform SQLite C API. No Python was added.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
npm ci --prefix console/web
npm run build --prefix console/web
```

Build the common package and each node independently **on the target Linux ISA**:

```sh
cmake --install build/dev --prefix "$PWD/build/node-sdk"
cmake -S docker-nodes/app-a -B build/app-a -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/node-sdk"
cmake --build build/app-a
cmake -S docker-nodes/app-b -B build/app-b -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/node-sdk"
cmake --build build/app-b
```

Each node uses only an installed package; neither build includes sibling sources. Linux sample binaries statically link libstdc++/libgcc and use the target system's glibc. The fixture Dockerfiles require a caller-supplied immutable Ubuntu-compatible base image and a build context containing `lab-node`:

```sh
mkdir -p build/image-app-a
cp build/app-a/lab-node docker-nodes/app-a/Dockerfile build/image-app-a/
docker build --network=none --build-arg BASE_IMAGE=sha256:BASE_IMAGE_ID \
  -t graphlab-m2/app-a:development build/image-app-a
```

Repeat for app-b. Use a digest returned by `docker image inspect` in the artifact lock. The reserved development reference `graphlab.local/NAME@sha256:IMAGE_CONFIG_ID` resolves directly to that local Docker image ID. Published `name@sha256:MANIFEST_DIGEST` references must already be installed and match inspected image identity/RepoDigests. A mutable tag is never resolved at runtime. Image presence/digest checks do not authenticate a publisher or prove a common-package supply chain.

`build/dev/m2_tests --fixtures SOURCE_DIRECTORY OUTPUT_DIRECTORY sha256:IMAGE_ID` generates a development-only Docker ring and artifact lock for testing. It initially uses the same supplied image for both nodes; replace app-b's reference if using its separate image and recompute the lock hash with `lab hash`. This generator inherits the M0 sample contract/package metadata, including synthetic package hashes; it is not a publication tool. Real workload publication must replace those metadata with verified package identities.

## Agent/API accounts and launch

The M1 invocation without `--state` remains read-only. To enable M2, run a root agent with a private state directory and an explicitly allowed operator/API UID. The API stays unprivileged and does not need Docker-group membership. Use trusted, root-owned installed agent binaries/configuration for persistent use. The temporary test build paths recorded in verification are not a production service installation.

For an operator whose UID is `501` and primary GID is `1000` (substitute actual values):

```sh
sudo install -d -m 700 /var/lib/graphlab
sudo install -d -m 750 -o root -g 1000 /run/graphlab
sudo /path/to/lab-agent \
  --socket /run/graphlab/agent.sock \
  --topologies /path/to/locked-m2-topologies \
  --lock /path/to/artifacts.lock.json \
  --state /var/lib/graphlab --allow-uid 501
```

The socket directory is owned by the agent and is not writable by the API group. The socket is root/group owned, mode 0660; the agent also verifies the caller UID. A state directory is required to be owned by the agent, mode 0700. Startup refuses an existing socket path. After a crash, verify that the old agent has stopped before removing its stale socket and restarting with the **same state directory**.

Provision the API credential as in M1, then run as the unprivileged operator. The complete flag order supported by the current CLI is:

```sh
lab-api --socket /run/graphlab/agent.sock --auth /private/auth.json \
  --assets /path/to/console/web/dist --port 8088 --agent-uid 0
```

Open `http://127.0.0.1:8088`, select a development topology and acknowledge the no-capture profile. Start/job state and quiesce/resume/destroy controls use the agent. A failed start offers **Recover and clean up**. Cancellation applies to start jobs only; teardown/recovery are allowed to finish.

## CLI and HTTP contracts

The CLI sends a method and a JSON parameter file through the same authenticated Unix RPC:

```sh
lab control --socket /run/graphlab/agent.sock --agent-uid 0 start start.json
```

`start.json`:

```json
{"topologyHash":"sha256:REPLACE_WITH_CATALOG_HASH","idempotencyKey":"unique-operation-key","developmentMode":true}
```

Other methods: `capabilities`, `topologies`, `diagnostics`, `runs` take `{}`; `run` and `job` take `{"id":"UUID"}`; `inventory` takes `{"hash":"sha256:…"}`; `cancel` takes a job `id`. `operate` takes:

```json
{"runId":"UUID","operation":"stop","expectedRevision":"1","idempotencyKey":"another-unique-operation-key"}
```

Operations are `stop`, `resume`, `destroy`, `recover`. Recovery means cleanup of the interrupted run, not retrying arbitrary effects. Destroy is repeatable with the current revision. Preserve/reuse the original key and identical parameters when retrying an ambiguous request. Keys remain until the journal is retired; there is no automatic history pruning or silent idempotency expiry.

HTTP adds `POST /api/v1/runs`, `GET /api/v1/runs`, `GET /api/v1/runs/{id}`, `POST /api/v1/runs/{id}/operations`, `GET /api/v1/jobs/{id}` and `POST /api/v1/jobs/{id}/cancel`. All POST routes require the logged-in session, exact Origin and `X-CSRF-Token`. Accepted jobs return 202 with `jobId`, `runId`, and revision; rejected static capabilities/parameters return 422, conflicts 409 and capacity failures 429. Runtime preflight happens in the job before the first resource effect and can fail an admitted job. No arbitrary command/path endpoint exists.

## Validation and limits

See [M2 verification](validation/m2-verification.md). Linux runtime tests create owned Docker/OVS resources and an unrelated sentinel, verify VLAN positive/negative controls, gate behavior, management separation, cleanup, and abrupt exits before/after each durable prepare step. They retain their SQLite/evidence directories for diagnosis. Run explicitly on a dedicated Linux lab:

```sh
sudo build/dev/m2_linux "$PWD" "$PWD/build/dev/m2_tests" \
  sha256:APP_A_IMAGE_ID sha256:APP_B_IMAGE_ID
```

The standard CTest suite never starts Docker or mutates networking. It covers durable journal/concurrency/cancellation/recovery behavior with a fault-injecting backend, lookup failure versus confirmed absence, namespace identity checks, and M0/M1 regressions. An authenticated HTTP router and the actual CLI race through one Unix RPC agent to check shared idempotency and conflict handling. `console/web/tests/m2-live.spec.ts` is separately gated by `GRAPHLAB_M2_LIVE` and requires the documented test harness's Linux services/tunnel on port 18089.

The live crash harness now covers before/after every Backend mutation call in start, stop, resume and destroy: preparation, activation, release/quiesce and removal. Still outside this release's qualification: every individual kernel/API sub-operation interruption, actual namespace replacement races and backend outage stress, exhaustive live graph/scale testing, GCC 14 and Linux x86-64. Namespace mismatch is tested with altered recorded identities against live containers; lookup failures and retained cleanup state use deterministic regression tests. This is not a claim that all possible instruction-level interleavings were covered. There is no M3 traffic lease, capture barrier, privileged worker adoption, background reconciliation, SSE event log, fault injection UI, QEMU execution or continuous telemetry. Agent downtime does not promise immediate quiescence. Deleting the state directory is not a cleanup operation.

Implementation references: [Docker Engine API 1.52](https://docs.docker.com/reference/api/engine/version/v1.52/), [OVS VLAN/RSTP database schema](https://www.openvswitch.org/support/dist-docs/ovs-vswitchd.conf.db.5.html).
