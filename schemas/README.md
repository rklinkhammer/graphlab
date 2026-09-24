# Implemented M0 contract

The authoritative validator is `packages/lab-support/cpp/contracts.cpp`. This documents the implemented subset; the larger option-D document is a design, not a claim that its future execution/console fields are accepted today. Unknown fields are rejected throughout.

## Document encoding

One YAML or JSON object, maximum 1 MiB, depth 64, and 100,000 values. Duplicate keys, aliases/anchors, merge keys, explicit tags (including standard explicit tags), complex keys and multiple documents are rejected. Scalars use strings, booleans spelled `true`/`false`, null, and decimal signed 64-bit integers. Quote digit-like values intended as strings. Floating-point fields are not part of M0. Identifiers match `[A-Za-z][A-Za-z0-9_-]{0,63}`. SHA-256 identities are `sha256:` followed by 64 lowercase hexadecimal characters.

## Topology: `graphlab.topology/v2`

Required: `apiVersion`, `id`, `artifactLock`, `backend`, `management`, `nodes`, `edges`, `capture`.

- `artifactLock`: SHA-256 of the parsed canonical artifact-lock object.
- `backend`: `{kind: linux-local, switchIsolation: shared-ovs}`. Other backends are rejected.
- `nodes`: map of IDs to nodes. Kinds: `docker`, `qemu`, `ovs-switch`.
- Workload nodes require `workload` referencing a lock key and `ports`. Optional `addresses` maps data-port names to IPv4 CIDRs. Switch nodes require `policy` with `forwarding: normal`, boolean `rstp`, and `priority` in 0..61440, divisible by 4096; they have no host IP addresses or workload reference.
- `ports`: map of names to `{role, medium?, mtu?, vlan?}`. Role is `data` or `management`, default medium is `ethernet`, default MTU 1500 (bounds 576..9216). Switch ports must be data and require exactly one VLAN policy: `{access: 100}` or `{trunk: [100, 200]}`. VLANs are unique integers 1..4094. Workload ports must match their contract's names/roles/medium/MTU range and cannot carry environment VLAN policy.
- `edges`: array of `{id, endpoints: ["node:port", "node:port"]}`. Unique edge IDs and exclusive endpoint allocation; compatible data roles/media/MTUs required. Endpoint order defines direction and is preserved.
- `management.networks`: map of `{subnet, dynamicPool, gateway, externalAccess: false}`. IPv4 networks are canonical, nonoverlapping, prefix 8..30; dynamic pools are contained; gateway and static attachments are usable addresses outside the dynamic pool.
- Optional `managementAttachments`: defaults to `[]`; entries `{endpoint, network, address}` attach a declared management port once, with unique static address/prefix in that network.
- `capture`: `{required: boolean, scope: all-data-edges, format: pcapng}`. Planner emits one capture per data edge, including blocked/idle links; runtime capture is not implemented.
- Optional `limits`: both `maxNodes` and `maxEdges`; defaults 1024 and 8192. Allowed limits 1..10000 nodes, 0..100000 edges (document size limits also apply).
- Optional `loopPolicy`: `rstp` (default) or `unprotected`. Cyclic switch components require RSTP on all component switches unless explicitly unprotected. Self-links require distinct switch ports and explicit `unprotected`. Isolated nodes, unused ports, disconnected components, rings, meshes and parallel edges are legal. Required workload interfaces must be declared, but need not be linked (an intentionally isolated node is valid).
- Optional `requiredFeatures`: only `[]` is currently supported; unknown required features fail closed.

Supported edge primitives: switch–switch, Docker–switch and Docker–Docker use veth; QEMU–switch uses TAP. Direct QEMU–Docker and QEMU–QEMU edges are capability errors until the attachment backend is implemented. No automatic bonding, tunnels, wireless media, IPv6 addressing or multi-host execution is implemented. VLAN mismatches may be intentional tests; validation checks values and endpoint framing, not a guarantee of application reachability.

## Workload: `graphlab.workload/v2`

Required: `apiVersion`, `id`, `kind` (`docker`/`qemu`), unique nonempty `platforms` (`linux/amd64`, `linux/arm64`; QEMU also supports `linux/ppc64le`), `interfaces`, `lifecycle`, `labSupport`, `resources`.

- `interfaces`: names map to `{role, required: boolean, medium: ethernet, mtuRange: [min,max]}`. Bounds 576..9216.
- `lifecycle`: `{gateUntilRelease: true, quiesce: supported|restart-required}`.
- `labSupport`: `{language: cpp23, version: major.minor.patch, packageSha256: digest}`.
- `resources`: integer `cpus` 1..1024 and `memoryMiB` 16..1048576.
- Optional `requiredFeatures`: only empty array supported.

Execution commands, config mounts, device permissions, health transports, console capabilities and durable release leases are future contract extensions, not currently enforced runtime guarantees. The M0 planner names abstract operations; it does not generate shell commands.

## Artifact lock: `graphlab.artifacts/v1`

Required `apiVersion` and `workloads` mapping workload IDs to:

- `contract`: embedded workload contract with matching `id`.
- `contractSha256`: digest of that exact parsed contract (object keys sorted; arrays retain order).
- `platform`: one of the contract's advertised platforms.
- Docker: `image` with immutable `name@sha256:...`; no VM fields.
- QEMU: `diskSha256` and `vm` containing versioned `machine` (e.g. `virt-8.2`), `firmwareSha256`, and `accelerator: kvm|tcg`; no image field.

M0 does not resolve registry manifests, retrieve disk images, prove KVM compatibility, or allocate host resources. Local hash integrity does not prove those artifacts exist or have been authenticated. Examples use clearly synthetic identities and are for validation/planning only.

## Plan: `graphlab.plan/v1`

`mode: dry-run`, `executable: false`, `topologyHash`, `artifactLockHash`, `canonicalTopology`, `summary`, dependency-ordered `steps`, reverse dependency `resourceTeardownOrder`, and `deferredChecks`.

Each step has `id`, `operation`, sorted unique `dependsOn`, and logical `resource` metadata. Deterministic Kahn ordering uses lexical ready-step IDs. Node/edge creation precedes policy, each edge capture precedes the capture barrier, then link enablement/convergence precede workload release/readiness. Teardown order describes resource order only; actual stop/compensation and runtime ownership require later milestones. Runtime names and directions are explicitly unresolved/unverified.

M4 guest lock metadata accepts explicit `kvm` or `tcg` acceleration, a versioned machine and firmware hash. Optional `kernelSha256`/`initrdSha256` and `sshUser`/`knownHostsSha256` must each be supplied as a pair. Actual binary and credential checks occur on the Linux executor; schema validation remains read-only. See [guest templates](../qemu-guests/README.md).

`application-edge-telemetry-v1.schema.json` describes optional endpoint-owned edge reports; the shared C++ validator additionally enforces uint64, histogram and temporal consistency, and the agent validates declared topology/instance ownership.

`source-control-v1.schema.json` describes authenticated source commands; runtime validation also enforces ownership, capability, declaration, byte limits and lifecycle state. See [source controls](../docs/source-controls.md).

`graphlab.process-log-snapshot/v1` is the agent-owned retained output contract documented in [process logs](../docs/process-logs.md); consumers cannot submit arbitrary log sources.
`graphlab.capture-lengths/v1` contains writer-owned decimal captured/original byte totals and truncated-packet counts for finalized PCAPNG segments; see [capture metadata](../docs/capture-inspectors.md).

## Message observations

[Structural v1 schema](message-observations-v1.schema.json); [identity, clock, size and cross-field rules](../docs/message-observations.md). The authoritative runtime validator is `cpp/messages/history.cpp`.
