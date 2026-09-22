# C++ Docker nodes

`app-a/` and `app-b/` are independently buildable C++23 fixture nodes. Each consumes the installed, versioned `LabSupport::lifecycle` package through `find_package`; neither reads sibling source directories. No separate Git repositories or published releases were created by this implementation.

Both start with their application gate held. The common C++ library provides release, quiesce and status controls and a UDP echo/probe fixture bound to the `data0` address. Other declared interfaces can be wired by the controller; the sample echo service uses only `data0`. There is no Python node support. The node has no Docker/OVS authority and receives no host control sockets.

Build each node on the target Linux ISA, copy its executable into a small Docker build context, and supply an immutable Ubuntu-compatible base image to its Dockerfile. The controller consumes the resulting immutable image reference and workload contract.

See [M2 build and invocation](../docs/m2-executor.md) and [shared package](../packages/lab-support/README.md). Future independent application repositories belong under their own directories here; OVS infrastructure and QEMU definitions remain outside `docker-nodes/`.
