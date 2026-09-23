# C++ Docker nodes

`app-a/` and `app-b/` are independently buildable C++23 fixture nodes. Each consumes the installed, versioned `LabSupport::lifecycle` package through `find_package`; neither reads sibling source directories. No separate Git repositories or published releases were created by this implementation.

Both start with their application gate held. The common C++ library provides release, quiesce and status controls and a UDP echo/probe fixture bound to the `data0` address. Other declared interfaces can be wired by the controller; the sample echo service uses only `data0`. There is no Python node support. The node has no Docker/OVS authority and receives no host control sockets.

Build each node on the target Linux ISA, copy its executable into a small Docker build context, and supply an immutable Ubuntu-compatible base image to its Dockerfile. The controller consumes the resulting immutable image reference and workload contract.

See [M2 build and invocation](../docs/m2-executor.md) and [shared package](../packages/lab-support/README.md). Future independent application repositories belong under their own directories here; OVS infrastructure and QEMU definitions remain outside `docker-nodes/`.

M3 rebuilds advertise `graphlab.traffic-lease=1` and use the common ten-second traffic lease. Required-capture runs use lease release/renewal instead of the M2 indefinite development release. An expired lease holds the application gate until a new capture barrier authorizes release. See [M3 behavior and measured limits](../docs/m3-captures.md).

M4 adds recorded Docker exec consoles without passing the Docker socket to a node or console worker. Shared C++ QEMU guest examples are kept separately under [qemu-guests/](../qemu-guests/README.md).

M6's [qualification workflow](../qualification/README.md) independently rebuilds both applications from copied node-only source trees against an installed common package. The selected Linux image archives, SDK and node source/binary bundles are retained under ignored `qualification/artifacts/linux-arm64-m6/`; their hashes belong to the qualification manifest. Keep that directory when transferring examples. No published image/repository or untested protocol-minor compatibility is implied by the local build.

The [M6 completion bundle](../docs/validation/m6/completion.md) adds complete local SDK releases 1.0.0 and 1.1.0 and all four independent app/version images. Select the exact installed SDK with `-DLAB_SUPPORT_VERSION=1.0.0` or `1.1.0` (default). Gate minors 0 and 1 interoperate in both directions under the frozen and current controllers. The current controller validates the actual wire major before releasing any node. Preserve `qualification/artifacts/linux-arm64-m6-completion/` alongside the original bundles; it contains the pinned SDK/image bytes, not just version names.
