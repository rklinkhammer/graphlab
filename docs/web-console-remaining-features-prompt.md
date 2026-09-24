# Implementation prompt: remaining Graphlab web console features

Continue implementing the remaining web console gaps in `docs/web-console-feature-parity.md`. Implement working, tested increments; do not stop at a proposal or count an unsupported field rendered as zero as completion. Preserve existing work and finish each increment from contract through dedicated Linux verification before starting the next.

## Establish the current baseline

Read applicable `AGENTS.md` instructions, `README.md`, the parity report, and:

- `docs/application-dataflow.md`
- `docs/application-telemetry.md`
- `docs/packet-history.md`
- `docs/m4-qemu-consoles.md`
- `docs/m5-telemetry-faults.md`
- `docs/m6-qualification.md` and `docs/m7-backends.md`

Inspect current code and tests before deciding what remains. The working tree may contain completed, uncommitted changes; preserve them. Treat older planning documents as historical where they conflict with current code.

Already implemented: node-linked consoles and bounded log snapshots; serial writer release; directional interface metrics; node-scoped application telemetry and an instrumented C++ echo fixture; finalized PCAPNG packet history with bounded persistence, pagination, recycling, rebuild and recovery; versioned application dataflow declarations with connected application/network views. Do not recreate those systems.

Reference repository: `https://github.com/rklinkhammer/graphx-docker.git`, previously inspected at `7cad4da8646eda302a005070228495c1aa87d89a`. Check `/tmp/graphlab-graphx-docker-reference`; otherwise create a separate reference checkout at that revision. Inspect relevant frontend, collector, control and persistence contracts. Record paths and metric semantics, and label source inspection separately from runtime verification. Do not substitute a similarly named repository. Do not copy inactive controls as working reference features: the earlier inspection found a disabled Fault control and an Inspect messages button without a handler.

Update the parity matrix to distinguish:

1. Verified reference behavior still missing from Graphlab.
2. Explicit Graphlab extensions requested by this prompt.
3. Unsupported or deferred capabilities with an exact reason.

Use the following order. **Begin with increment A and complete its acceptance criteria before B.** If a later increment depends on unavailable external infrastructure or an unresolved semantic decision, record the specific blocker and continue independent authorized work. Do not invent observations to satisfy the UI.

## A. Application-edge telemetry and declared protocol metadata

Build on `graphlab.application-dataflow/v1` rather than introducing a competing application graph.

- Define compatible, versioned metadata for application protocol, framing and schema identifiers. Treat them as declarations, never inferred protocol validation. Bound all strings and collections; validate identifiers and references. Preserve existing topology hashes when no new fields are supplied and maintain deterministic canonicalization.
- Define workload reporting for named streams/application edges, including how source and destination reporters identify the same declaration. Bind reports to the agent-owned run, workload instance, stream and reporting epoch. Reject spoofed endpoints, unrelated edges, duplicate/conflicting reports and unsupported versions.
- Preserve node-scoped v1 reports and uninstrumented workloads. Do not distribute node totals across application edges or double-count reports from both endpoints.
- Report messages, payload bytes, errors, rejection, backpressure and reconnects only where the workload measures them. Define reconnect semantics for a connection-oriented fixture; show unsupported for transports without that concept.
- Define counter ownership, sequence rules, resets, stale/gapped observations and aggregation. Counters must remain exact above JavaScript's integer precision limit.
- Explicitly name latency measurements and their boundaries. Local processing time and requester-measured round-trip time are different metrics. Prefer monotonic measurements on one process; do not claim one-way network latency without a clock synchronization/error-bound contract. Define histogram aggregation, windowing, sample count and overflow behavior.
- Extend bounded agent ingestion, persistence, authenticated run/edge-scoped queries and the application-edge inspector. Keep application metrics separate from interface statistics. Shared network resources must not cause their counters to be attributed to individual application flows.
- Instrument at least one C++ workload fixture with two distinguishable streams so tests can detect cross-stream attribution. Keep a legacy workload in the integration fixture.

Acceptance: contract rejection and migration tests; exact counter/rate and epoch tests; row/byte/retention limits; authorization and scope isolation; browser selection/staleness tests; dedicated Linux traffic proving independent stream counts against fixture-owned observations. Document which measurements are unavailable and why.

## B. Source-specific pause/resume

Add a versioned, capability-advertised workload control contract. Whole-run quiesce/resume must retain its existing semantics.

- Define the controlled source/stream, what stops being generated, what continues receiving/processing, how in-flight work is handled, and what acknowledgement means.
- Add a real controllable source to a C++ fixture. Do not label an echo receiver or an entire container stop as source pause.
- Route controls through the existing authenticated agent/runtime ownership model. Bind requests to the selected run, node, workload generation and source identity. Preserve Origin/CSRF checks.
- Bound command queues and retained status. Use request IDs/idempotency and explicit requested, acknowledged, failed, timed-out and interrupted outcomes. Define restart reconciliation and prevent stale commands from controlling replacement workloads.
- Keep run quiescence authoritative: source resume must not release a quiesced run or bypass capture-first admission. Preserve receive/management/control paths according to the contract.
- Render controls only for advertised capabilities; show target identity, pending state and outcome. Protect against late responses after selection changes.

Acceptance: duplicate commands, wrong identity, unsupported workload, timeout, restart, stale UI response and authorization tests; Linux proof that one source stops and resumes generation while another source and the existing capture process continue; explicit run-quiesce precedence checks.

## C. Durable process-log artifacts

Extend the current bounded log snapshots where durable retention is missing. Reuse existing runtime log sources; do not create another serial reader or mislabel shell recordings as process logs.

- Define source identity, generation, byte encoding, timestamps, ordering/cursors, attachment boundary and gap/truncation semantics for Docker logs, QEMU worker journals and container-runner stderr where available.
- Use one owned, bounded collection path with backpressure isolation. Never block capture, execution or terminal recording on the log consumer.
- Persist bounded artifacts with explicit per-source/global byte limits, retention, restart behavior, atomic finalization and disk-full handling. Avoid unbounded duplication of runtime-managed logs.
- Preserve retained access after container removal/run destruction when collection succeeded. State explicitly that collection cannot recover bytes already discarded before attachment.
- Expose authenticated run/node/source queries and integrity-checked retained downloads. No arbitrary filesystem paths or unit/container identifiers supplied by the browser.
- Extend the existing viewer with durable/live source distinction, exact-byte download, frozen older pages, return-to-newest, search scope, gaps and availability. Render untrusted output safely.

Acceptance: rotation, source restart, duplicate replay, invalid byte sequences, truncation, exhaustion, agent restart and destruction tests; Linux Docker and QEMU source verification with independent byte comparisons; browser tests for scope changes and retained access.

## D. Evidence-backed inspectors and capture metadata

Audit the current inspector/catalog against the reference before adding fields.

- Present declared application protocol/framing/schema and associated network resources from A with provenance.
- Present existing runtime mappings, administrative/carrier/RSTP state, shared failure domains, capture coverage and freshness independently. A configured association or enabled link is not proof of reachability or delivery.
- Add diagnostic details only from identifiable observations. Separate measured failure evidence from explanatory hypotheses; do not synthesize a definitive failure layer from missing telemetry.
- Fill remaining capture catalog gaps using manifest/file metadata actually supported by the capture pipeline: capture node/interface, link type, format, finalization time, captured/original lengths or truncation summaries where recorded, limits and verified downloads. Label filesystem modification time separately from capture time if exposed.
- Keep selection navigation consistent across application edges, workload nodes, associated network edges, captures and packets. For a set of links, require explicit selection or a documented union query; do not silently choose one or duplicate totals.

Acceptance: metadata-to-source tests, wrong-run and stale-generation tests, unknown/missing-state browser coverage, real Linux artifact/manifest comparisons, and preservation of checksum failures and existing download behavior.

## E. Explicit application-message observations and supported capture correlation

This is a new opt-in workload/backend increment, not an inference from existing aggregate reports. Keep it separate from packet observations and complete A before starting it.

- Define bounded, versioned message observations: run/workload/stream identity, reporter epoch, message/trace identity, event kind, sequence, payload length and timestamp/clock domain. Define uniqueness, reuse, retries and fan-out semantics.
- Keep payload recording disabled by default. Metadata limits, sampling and dropped/omitted observation counts must be visible. Instrument a controlled fixture that reports actual send/receive events.
- Design independent bounded ingestion, persistence, restart recovery, stable pagination and authenticated scope. Browser history must freeze older pages and expose omissions.
- Correlate to captures only when the fixture's documented wire format contains a verifiable identifier and the retained, checksum-verified bytes support an exact match. A tuple and nearby timestamp are insufficient proof.
- Start with an explicitly supported datagram format and precise artifact/packet/block references. Represent ambiguity, retransmission/duplication, snapshot truncation, encryption and unsupported formats as unavailable or ambiguous. Do not invent TCP stream reassembly or universal correlation.
- Define delivery accounting separately. Missing capture/message observations are not proof of loss; only expose measured delivery/loss where the endpoint protocol, observation coverage and completion/deadline rules justify it.

Acceptance: known fixture traffic checked against endpoint observations and actual retained PCAPNG bytes; duplicate/reused IDs, dropped observations, truncation, incomplete capture, unrelated traffic, restart, wrong-run and pagination tests. Browser tests must demonstrate unavailable/ambiguous outcomes as well as exact matches.

## F. Complete qualification and reconcile parity claims

- Verify every new backend/workload feature in the dedicated Linux environment, using isolated source/build/state, uniquely named services and reserved ports. Discover current availability first; do not assume earlier fixture services or images still exist.
- Run the relevant native contract/integration tests, production console build and browser tests. A skipped opt-in test is not verification. Test real HTTP/agent authorization as well as mocked presentation.
- Qualify relevant Docker and QEMU serial/log behavior, management connectivity, retained artifacts, replay/gaps, restart and teardown. Guest images must provide any claimed login service. Preserve single ownership of serial and capture inputs.
- Include meaningful queue/byte/record bounds, slow consumer, restart/crash and actual isolated filesystem-exhaustion tests where new persistence is introduced. Do not reuse destructive tests on ordinary state directories.
- Exercise application/network selection, parallel/reverse/self edges, disconnected/isolated workloads, stale data, legacy workloads, narrow layouts, keyboard navigation and session expiry. Inspect screenshots for changed views.
- Record the commands, revision/source paths, environment, fixture/image identity, test results, live source comparisons, screenshots and cleanup evidence in `docs/validation/`. Never record credentials, cookies or private tokens.
- Destroy only task-created runs and stop only task-owned services. Retain verification artifacts according to their documented policy and preserve existing recovery quarantines.
- Update `docs/web-console-feature-parity.md`, contract/operator docs and README links after each increment. Separate fresh evidence from historical results; list every remaining limitation. Do not claim full parity while required rows are blocked or unverified.

## Invariants for all increments

Use C++23 for project-owned backend/runtime code and TypeScript/React for the console. Reuse existing ownership, protocol, lifecycle and persistence abstractions; no parallel Python/Node backend, duplicate capture process, second serial reader or unrelated architectural rewrite.

Preserve capture-first guarantees, immutable run/topology identity, verified downloads, serial writer fencing, terminal byte safety, existing fault controls and legacy workload compatibility. Treat optional telemetry/log/history failures as visible subsystem failures without silently changing execution behavior.

Keep these concepts distinct: declared application relationships, observed network resources, interface counters, application counters, message events, packet observations, and verified correlation. Unsupported is not zero; stale is not current; absence of observation is not loss.

Active/unmanifested capture indexing, arbitrary imported PCAP formats, general payload inspection/TCP reassembly, universal direction attribution, synchronized one-way latency and dynamically discovered routes are **not required by this prompt**. Keep their current exclusions unless separately authorized; the finalized-segment integrity/lifecycle boundary remains in force.

For each increment, deliver a concise explanation of the final behavior, changed contracts, tests actually run, evidence locations, remaining limitations and the next unfinished increment. Continue within this scope without asking for approval of routine implementation choices.
