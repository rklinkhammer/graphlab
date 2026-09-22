
Act as a systems architect. Explore architectures for a reproducible simulation environment where applications run in Docker containers and Open vSwitch (OVS) manages networking. Some workloads will be virtual machines running under QEMU, potentially with QEMU itself running inside a container.

The environment must model physical network switches, so OVS is a core requirement. Each containerized application or workload definition must live in its own Git repository, with its own image build and versioning lifecycle. A separate repository may own topology definitions, orchestration, and integration tests.

Your task is to compare architectures and recommend a practical starting point. Do not implement anything yet.

Implementation and layout requirements:

- Design the environment controller and custom orchestration components for C++23. Include a concrete implementation design and phased implementation plan in the study; production code is outside the scope of this study.
- Implement any custom lab support running inside Docker nodes in C++23 as well, including lab agents, lifecycle helpers, configuration adapters, readiness/health checks, and telemetry or experiment hooks. This requirement applies to lab support code, not the application workload's implementation language.
- Where practical, factor reusable lab support into common C++23 libraries shared across Docker nodes and, where appropriate, the controller. Keep node-specific adapters small and avoid duplicating common behavior. Treat sharing as reuse of versioned library packages; choose static or dynamic linking based on deployment needs rather than requiring shared-library binaries.
- Minimize Python use. Prefer C++23 for topology parsing and validation, workload lifecycle management, network setup, failure injection, cleanup, and experiment execution. Identify any unavoidable Python dependencies in candidate tools, explain their role, and evaluate alternatives. Do not introduce a parallel Python controller or require Python for the recommended design's core runtime unless a justified dependency makes it necessary.
- Keep Docker-node definitions in a dedicated `docker-nodes/` directory, separate from controller sources, topology definitions, and OVS infrastructure. Each application node must retain its own Git repository and image build/version lifecycle. Show `docker-nodes/<node-name>/` as optional independent checkouts in a development workspace, not as workload source copied into the controller repository. Runtime orchestration must work from published immutable images without those checkouts. Distinguish application nodes from containers hosting QEMU or OVS.
- Support arbitrary user-defined topologies rather than a fixed layout or node count. Model topology as a graph with explicit nodes, named interfaces/ports, and links between endpoints. Support chains, stars, rings, meshes, disconnected components, redundant paths, and parallel links using distinct ports, with arbitrary placement of supported Docker and QEMU workloads. State practical resource limits and unsupported combinations explicitly. Validate unique identifiers, endpoint references, port allocation, and link compatibility; permit intentional cycles and isolated nodes rather than treating them as invalid.

Clarify these distinctions before evaluating options:

- Application containers, QEMU virtual machines, containers that host QEMU, and OVS infrastructure containers have different lifecycle and networking requirements.
- Distinguish a simulated physical switch, an OVS bridge, an OVS process, and a network namespace. Explain which isolation boundaries each proposed architecture uses.
- Explain what OVS can faithfully represent and where it differs from physical switches, including hardware forwarding pipelines, buffering, congestion, timing, and vendor behavior.
- Separate workload management, network topology management, and experiment execution.

Explore at least these architectures:

1. A single Linux host with Docker, host-managed OVS, and a custom topology controller.
2. Containerlab or a comparable network-lab orchestrator integrating Docker, OVS, and QEMU.
3. Mininet or Containernet with Docker workloads and an approach for integrating QEMU.
4. A declarative custom orchestrator using Docker APIs, Linux network namespaces, veth pairs, TAP interfaces, and OVS.
5. A multi-host architecture, using Kubernetes or another scheduler only where its benefits justify the complexity.

For each option, explain:

- Component placement, ownership, and control flow.
- How containers and QEMU guests connect to simulated switches, including veth, TAP, bridges, and network namespaces.
- Whether a simulated switch uses its own OVS instance or shares an instance, and the consequences for isolation, failure injection, and observability.
- How management access stays separate from simulated data-plane traffic.
- Support for VLANs, trunks, L2 loops, STP/RSTP, link aggregation, tunnels, and optional SDN control; verify support rather than assuming it.
- How latency, loss, bandwidth limits, link failures, and switch failures are introduced, and how realistic those effects are.
- QEMU acceleration with KVM, device access, privileges, image distribution, and guest lifecycle management.
- Reproducibility, startup ordering, readiness checks, cleanup, recovery after partial failures, and prevention of orphaned interfaces or OVS state.
- Packet capture, metrics, logs, topology inspection, and experiment-result collection.
- Single-host and multi-host scaling limits, resource contention, and implications for timing fidelity.
- Development and CI requirements, including Linux versus macOS/Windows and nested virtualization constraints.
- Operational complexity, security boundaries, project maturity, and maintenance burden.
- Suitability for a C++23 controller, required integration interfaces, Python dependencies, and support for arbitrary graph topologies without generated code or topology-specific orchestration logic.

Address the independent-repository requirement explicitly. Propose:

- A sample repository structure for an application workload, a QEMU workload, and the environment controller.
- A common workload contract describing network interfaces, configuration, health checks, resource requirements, image references, and supported lifecycle operations.
- A topology schema that refers to immutable workload versions without requiring repositories to be checked out together.
- A build, publish, compatibility-testing, and dependency-update workflow across repositories.
- An independently versioned common C++23 lab-support library repository, with published CMake-consumable packages pinned by each node repository. Explain API compatibility, reproducible dependency resolution, and coordinated updates without requiring repositories to be checked out or released together.
- A clear division between workload-owned configuration and environment-owned wiring.

Describe the C++23 implementation design explicitly:

- Propose module boundaries for the workload contract, topology graph and schema validation, planning/reconciliation, Docker integration, QEMU process management, Linux networking, OVS integration, experiment execution, and observability.
- Identify reusable node-support components such as configuration/contract parsing, lifecycle and readiness reporting, logging/metrics, and experiment messaging. Explain which can be shared with the controller, keep privileged host orchestration separate from node support, and show how two independently built Docker nodes consume the same versioned libraries. Include compatibility tests and an in-node acceptance check that custom lab support runs without Python.
- Show a sample layout with `cpp/`, `include/`, `topologies/`, `tests/`, and `docs/` in the controller repository, plus the separate `docker-nodes/` workspace directory containing independent node repositories. Explain where QEMU workload repositories and OVS infrastructure definitions live.
- Specify a CMake-based build, compiler and standard-library requirements, dependency management, and Linux runtime requirements. Distinguish portable parsing/planning code from Linux-specific execution. Verify support for any C++23 features the design relies on.
- Compare direct APIs with controlled subprocess integration for Docker, QEMU, OVS, and Linux networking. Explain error propagation, deadlines, cancellation, readiness checks, resource ownership, and idempotent recovery after partial failure. Avoid shell interpolation of topology or workload input.
- Explain how the controller turns an arbitrary topology into a dependency-aware execution plan, tracks created resources, and tears them down safely. Keep orchestration algorithms independent of the example topology.
- Propose C++ unit and integration tests, with shell or existing tool-based CI glue only where useful. Identify any remaining Python use and why it cannot reasonably be eliminated.

Use a concrete example throughout: two application containers and one QEMU guest connected through three simulated physical switches, with redundant links and a separate management network. Show how each architecture would represent and launch this topology. Treat it as one instance of the general schema, not a fixed architecture; also show how a chain, a ring, and a disconnected topology can be expressed through configuration alone.

Deliver:

1. The most important unanswered requirements, with explicit assumptions that let the analysis proceed.
2. Architecture diagrams and concise explanations for each viable option.
3. A comparison matrix covering fidelity, Docker/QEMU integration, reproducibility, complexity, isolation, scaling, and CI suitability.
4. A recommended initial architecture and the conditions that would justify choosing another.
5. An illustrative topology definition and workload contract for the recommended design, including explicit ports, arbitrary graph connectivity, immutable image references, and the dedicated Docker-node directory layout.
6. A phased C++23 proof-of-concept implementation plan with acceptance criteria, including connectivity, isolation, VLAN behavior, redundant-link behavior, failure injection, packet capture, and complete cleanup. Include configuration-only tests for chains, stars, rings, meshes, disconnected components, and parallel links; invalid endpoint/port rejection; and cleanup after partial startup failure.
7. The largest technical risks and small experiments that would validate or reject the recommendation.
8. A C++23 implementation blueprint covering controller and in-node lab support, common support libraries and their cross-repository distribution, integration interfaces, build/dependency choices, and an explicit inventory of any Python dependencies with their justification.

Use current primary documentation to verify tool capabilities and cite sources. Clearly distinguish native capabilities, custom integration work, and uncertain or untested assumptions. Favor the simplest architecture that satisfies the requirements, and explain where greater simulation fidelity would require something beyond OVS.
