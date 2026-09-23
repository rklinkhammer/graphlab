# Prompt: Graphlab web console feature parity

Update the existing Graphlab web console to support the operator-facing features of the web console in **graphx-docker**, especially interactive serial consoles and link performance metrics. Implement and verify the changes; do not stop at a design proposal. Preserve Graphlab's working runtime, capture, terminal, telemetry, and security contracts.

## Establish the reference and implementation baseline

- Target repository: `/Users/rklinkhammer/workspace/graphlab`.
- Reference repository: **https://github.com/rklinkhammer/graphx-docker.git**, inspected at commit **`7cad4da8646eda302a005070228495c1aa87d89a`**. A read-only reference checkout was created at `/tmp/graphlab-graphx-docker-reference`; if it no longer exists, clone the repository and inspect that revision. Do not assume the sibling `GraphX` repository is the same project. Record any deliberate change to the reference revision.
- Read applicable `AGENTS.md` instructions. Inspect the reference console's frontend, backend routes, terminal transport, metric collectors, documentation, and tests. If runnable, inspect its operator workflows in a browser. Record the reference revision and source paths.
- Produce `docs/web-console-feature-parity.md` with a table of reference feature, source evidence, current Graphlab support, required change, and acceptance test. Distinguish verified reference features, explicit requirements from this prompt, and optional improvements. Include other relevant operator features discovered in the reference; do not invent its capabilities.
- Inspect current code before deciding what to build. Graphlab already implements substantial functionality:
  - `console/web/src/main.tsx`, `graph.ts`, and `style.css`: React Flow topology viewer, selection, management layer, authentication, and diagnostics.
  - `console/web/src/execution.tsx`: run lifecycle, jobs, capture artifacts, and verified downloads.
  - `console/web/src/terminal.tsx`: xterm.js terminals, recorded output replay, writer leases, and SSH/Docker session creation; existing serial sessions appear in the session selector.
  - `console/web/src/telemetry.tsx`: directional rates, bounded history charts, netem preview/apply/remove, and correlated timeline.
  - `cpp/console/`, `cpp/terminal/`, `cpp/telemetry/`, `cpp/runtime/`, and corresponding `include/graphlab/` headers: existing backend contracts and execution ownership.
- Read `README.md`, `docs/m4-qemu-consoles.md`, `docs/m5-telemetry-faults.md`, `docs/m6-qualification.md`, and `docs/m7-backends.md`, plus relevant tests. Treat `docs/web-console-architecture.md` and `docs/option-d-console-plan.md` as historical planning material where they conflict with current implementation. Preserve C++23 for project-owned backend/runtime code and TypeScript/React for browser code; do not introduce a second Python or Node backend.

## Required operator experience

### Verified reference feature inventory

The following features were established by source inspection at the revision above, not by running the reference console. Paths in this table are relative to graphx-docker. Use them as the initial parity checklist, and trace the associated backend and tests before implementing equivalents.

| Reference feature | Source evidence | Graphlab requirement |
|---|---|---|
| Application / Network path / History / Capture navigation; live connectivity and traffic status | `web/src/App.jsx`, `web/src/useTelemetry.js`, `apps/telemetry/topology.mjs` | Provide connected views with clear observed-versus-configured state. Derive application/network relationships only from declared contracts and runtime mappings; do not invent application edges. Preserve Graphlab's management layer. |
| Click-node console dock with node selector | `web/src/App.jsx`, `web/src/components/NodeConsolePanel.jsx` | Open the selected node's logs and available consoles directly from the graph. |
| Searchable logs, pause/resume scrolling, retained-output download, stale/gap and generation/runtime indicators, QEMU process diagnostics | `web/src/components/NodeConsolePanel.jsx`, `apps/telemetry/node-console.mjs`, `src/infra/node_console.cpp` | Add bounded node log retrieval and a log viewer with these functions. Distinguish workload/process logs from interactive shell recordings. Label retention/truncation and source. |
| QEMU serial terminal with acquire/release keyboard authority, bounded input, retained output, generation resets, and gap detection | `web/src/components/NodeConsolePanel.jsx`, `web/src/nodeConsole.mjs`, `apps/telemetry/node-console.mjs` | Use Graphlab's existing serial worker, replay protocol, and writer leases; add explicit release as well as acquire/takeover. Do not copy the reference's fixed ttyS1 or 80×24 assumptions into all Graphlab guests. |
| Edge badges and inspector: throughput, sent/received counts, mean/p95 latency, byte rate and totals, errors/drops, reconnects, backpressure count/time, rejected messages | `web/src/components/TelemetryEdge.jsx`, `web/src/components/EdgeInspector.jsx`, `apps/telemetry/metric-store.mjs` | Implement the relevant inspector fields with metric provenance and availability. Keep packet, application-message, interface-byte, and application-payload semantics distinct. Application-only fields require workload instrumentation, not inference from interface counters. |
| Protocol/schema/framing, configured network path, diagnostic evidence and failure layer | `web/src/components/EdgeInspector.jsx`, `apps/telemetry/topology.mjs`, `apps/telemetry/runtime-evidence.mjs` | Surface declared metadata and verified runtime diagnostics with their source; do not treat configured connectivity as proof of delivery. |
| Recent message/packet observations and capture references | `web/src/components/EdgeInspector.jsx`, `apps/telemetry/metric-store.mjs` | Add bounded observation details and exact capture correlation where a real source supports them. Preserve packet/message identity distinctions; show unavailable for aggregate-only sources. |
| Durable event history or bounded TCP/UDP packet history, older-record pagination, return-to-newest, storage/drop/queue/retention status | `web/src/components/HistoryPanel.jsx`, `apps/telemetry/history.mjs`, `apps/telemetry/http-routes.mjs` | Expand the current timeline into a usable paginated history view. Packet history requires an explicit bounded collection/storage contract; it is not equivalent to rate history. |
| Capture catalog with format, node, link type, size, modification time, limits/truncation and downloads | `web/src/components/CapturePanel.jsx`, `apps/telemetry/capture-files.mjs` | Enrich Graphlab's capture catalog using real artifact metadata while retaining checksum verification and coverage status. Never mislabel Ethernet, raw application, or GraphX records. |
| Source pause/resume, counter reset, command status | `web/src/App.jsx`, `web/src/components/ControlCommandStatus.jsx`, `apps/telemetry/control.mjs` | Map source controls only to semantically equivalent supported operations; whole-run stop is not automatically source pause. Implement a clearly scoped display baseline/reset if needed without erasing retained evidence or resetting host counters. Show command acknowledgement/failure. |

Reference limitations matter: the **Fault unavailable** toolbar button is disabled, and **Inspect messages** in `EdgeInspector.jsx` has no click handler. Do not count these as functioning features to copy. Graphlab already has real directional netem controls and must retain them. The reference serial login requires a suitable guest image; a browser console does not provision a login service. The reference uses measured events or cumulative application reports with explicit unavailable fields; it does not provide universal latency or per-packet history for every edge.

Searchable logs, node-driven serial access, edge overlays/inspectors, history navigation, capture catalog details, and status indicators are required parity work. Track application-instrumented metrics, message identities, packet observation, and source-level controls as explicit backend/workload gaps where absent. Implement compatible extensions within scope and document any genuinely blocked capability individually; do not silently replace these requirements with interface rates or a generic timeline.

### 1. A connected topology and run workspace

Make topology, selected run, selected node/link, consoles, performance, captures, faults, and events work together. Selecting a node or link on the graph must open the relevant inspector and actions without requiring the operator to rediscover the same resource in another dropdown. Keep the topology and run identities consistent; never show metrics or send actions for a different run than the selected graph.

Support arbitrary validated topologies, including parallel edges, disconnected and isolated nodes, supported QEMU connections, and the separate management layer. Preserve node positions, zoom, and selection across telemetry refreshes. Provide clear node/link state indicators and readable endpoint/interface labels. Keep administrative state, carrier state, RSTP state, and observation freshness distinct. Expose shared OVS failure domains accurately.

### 2. Node consoles, especially serial

Provide obvious node-level actions for **Serial console**, **SSH console**, and **Container shell**, gated by actual backend capabilities. Attach to the existing run-owned QEMU serial session; do not create a second reader of its serial socket. Serial must remain usable before guest networking or SSH readiness. Explain unavailable actions using backend-supported reasons. Show structured diagnostics for switches without shell capabilities.

Support multiple open console tabs or equivalent persistent session views, labeled by node and console kind. Switching panels must not terminate backend sessions or lose session identity. Include terminal resizing where supported, reconnect/replay, connection status, writer/viewer status, lease acquisition and takeover, and explicit shell close. Respect serial lifetime: it belongs to the VM and must not be closed as an ordinary shell.

Add a Logs view alongside interactive consoles: search loaded output, pause/resume auto-scroll, download retained bytes, show source and retention limits, and surface QEMU process diagnostics separately from guest serial output. Release keyboard authority explicitly without terminating the console. Scope log APIs to authorized run/node identities and bound output size; never accept arbitrary host log paths.

Preserve byte-safe xterm rendering and the existing terminal protocol. Prevent duplicate replay output and stale input after run/session changes. Display recording status, exact-input opt-in status, and any reported gaps or incomplete recordings. Reconnect as a viewer until writer authority is acquired. Preserve retained recording access after teardown. Do not claim first-byte boot recording: the current documented guarantee is coverage from attachment.

### 3. Link performance metrics

Add live directional rate overlays to the topology and a detailed inspector for the selected link. Show A→B and B→A with explicit endpoint names, human-readable units, timestamps/freshness, and time-series charts with axes, legends, and inspectable values. Keep selection synchronized between graph, metrics table, inspector, captures, and faults.

Expose available byte/packet counters, bit/s and packet/s rates, errors/drops, interface state, qdisc observations, counter source, and mapping epoch as supported by real collected data. Extend the existing typed telemetry/API contracts where necessary. Use one canonical counter source per logical edge; never sum both endpoints as separate traffic. Preserve decimal-string precision for large counters and timestamps. Counter resets, identity changes, first samples, collection failures, and missing intervals must create unavailable values and chart gaps, not zero traffic or spikes.

Support existing history resolutions and retention limits, show truncation, and bound polling and chart data. Use a shared source of telemetry for overlays and inspectors so multiple panels do not multiply collection load. Handle requests completing after run or selection changes without displaying stale results.

Keep measured metrics separate from configured impairments. Configured netem delay/loss is not measured latency/loss. Report latency, jitter, observed loss, or utilization only when supported by a defined measurement source or known capacity. If the reference offers measurements absent in Graphlab, implement a scoped backend measurement facility with documented semantics and tests, or record the precise unresolved gap; never synthesize convincing values or assume link capacity. Active probes must be an explicit operator action because they introduce traffic.

### 4. Integrate existing operations and discovered reference features

Make run/job progress, cancellation, captures and checksum-verified downloads, terminal recordings, directional fault controls, and correlated events accessible from the relevant selection. Keep preview-before-apply, revision/idempotency checks, fault-placement limitations, expiry/recovery status, capture coverage, and retained evidence visible. Preserve read-only mode with capability-based controls.

Implement additional reference console features that fit Graphlab's operator scope, using the parity table to track each one. Features that require new topology authoring, orchestration semantics, or unrelated infrastructure need a documented scope decision; do not silently expand the project or mark unsupported features complete.

## Engineering constraints

- Reuse the existing authenticated API, agent, supervised workers, persistence, and ownership model. Extend them only where required. Keep CLI/API behavior compatible.
- Preserve authentication, Origin/CSRF enforcement, terminal writer fencing, output recording bounds, and artifact authorization. Never expose Docker, QMP, private credentials, host commands, or privileged sockets to browser code.
- Refactor the compact frontend into maintainable typed components and shared data/state hooks as needed. Include accessible labels, keyboard navigation, focus handling, responsive panels, loading/empty/error states, and clear stale/unsupported states. Keep operational values and actions usable without color alone.
- Do not redesign or replace working execution/capture infrastructure merely to reproduce the reference's implementation choices. Reproduce useful behavior within Graphlab's architecture.

## Acceptance and delivery

Implement in reviewable increments: evidence and parity map; shared run/selection state and inspectors; consoles; metric overlays/charts; remaining verified feature gaps; integration verification and documentation. Continue through implementation after creating the map.

Add meaningful automated coverage using existing Playwright and C++ test infrastructure. Verify:

1. A graph node opens the correct serial, SSH, or container console; unavailable modes explain why. Serial output works without guest networking.
2. Input reaches only the selected authorized session. Resize, writer expiry/takeover, tab switching, reconnect/replay, teardown, and retained recordings behave correctly. Browser disconnect does not terminate run-owned workers.
3. Graph edge selection drives matching overlays, history, capture artifacts, and fault controls. Known one-way traffic produces the correct direction and units without double-counting; parallel links stay distinct.
4. Idle traffic is distinguishable from unavailable/stale data. Counter reset, collector failure, history gaps/truncation, and out-of-order request completion do not produce misleading charts or cross-run data.
5. Read-only operation, authentication, rejected mutations, unsupported fault placement, capture workflows, and existing run/job controls continue to work.
6. Panels remain usable at representative desktop and narrow widths. Run switching and repeated panel/console opening do not leak sockets, timers, subscriptions, or terminal instances.
7. Log search, scroll pause/resume, retained download, generation change, and truncation indicators behave correctly. History pagination does not overwrite older records with background refreshes; capture catalog metadata matches the downloaded artifact.
8. Instrumented application metrics retain their actual units and provenance. Aggregate-only workloads do not acquire invented latency, message identities, or packet records. Source control and counter display reset preserve run/capture ownership and retained evidence.

Run `npm run build --prefix console/web`, the relevant Playwright suite (`npm test --prefix console/web`), and appropriate CMake/CTest checks for backend changes. Use the documented Linux qualification environment for real Docker/QEMU/OVS behavior; mocked browser tests alone do not establish runtime parity. Inspect the finished interface in a browser and retain useful screenshots. If runtime prerequisites are unavailable, complete runnable checks and report the exact unverified scenarios without claiming they passed.

Update operator documentation and the parity table with implemented behavior, evidence, remaining gaps, and qualification limits. Deliver a concise summary of changes, tests/results, and any reference features that remain unsupported. Do not declare full parity until every in-scope reference feature has implementation and verification evidence.
