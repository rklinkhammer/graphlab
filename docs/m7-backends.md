# M7 optional backends

M7 adds direct guest attachment plumbing and an opt-in C++ QEMU container runner. The shared OVS backend remains the default. Qualification results and exact runtime/image identities are recorded under `docs/validation/m7/`; M6 evidence is preserved separately and does not certify these additions.

## Direct guest links

Guest–Docker and guest–guest edges use one owned, two-port OVS attachment bridge per edge. This bridge is internal plumbing, not a logical topology switch. Its UUID, host ports and ifindices are recorded in the edge identity. It has no host IP. Creation follows the existing durable edge intent; cleanup verifies ownership and removes attachment resources before guest TAPs and endpoint containers. Parallel links use separate attachment resources and physical ports.

Each modeled edge still has one required capture. A guest TAP is the canonical observation point: TAP RX is guest-to-network and TAP TX is network-to-guest. A captured packet is not proof of endpoint delivery. Direct-link fault placement uses the Docker endpoint's egress for Docker-to-guest traffic and the opposite host attachment's egress for guest-originated traffic. Shared-switch guest egress still requires the unimplemented IFB capability and is rejected.

## QEMU container runner

Add `vm.runnerImage: "sha256:..."` to a QEMU workload's artifact-lock entry, then set the topology's `artifactLock` to the output of `build/dev/lab hash artifacts.lock.json`. Omitting it uses host QEMU. Mutable image tags are rejected by the shared validator; preflight checks image ID, native host architecture and the runner protocol label. Guest disk, firmware, machine and accelerator remain pinned independently.

The image starts `lab-qemu-runner`, a C++23 PID-1 supervisor using the same QEMU argument builder as host execution. QEMU starts paused. The existing independent terminal worker records serial output, controls QMP and enforces capture leases. A separate container watchdog terminates QEMU if the terminal heartbeat is lost; a killed worker cannot leave an indefinitely running guest. Controller restart does not kill a healthy terminal worker.

This is packaging and process supervision, **not network or kernel isolation**. The runner explicitly uses the host network namespace to access scoped TAPs, drops capabilities except `NET_ADMIN`, receives only the tun/KVM devices it needs, has a read-only rootfs, and mounts only its VM directory and digest-verified artifact files. It receives no Docker socket. Containers have deterministic names and run/resource/generation labels; recovery checks these and the recorded container ID before removal.

On the qualified ARM64 Linux build host, build an offline image from the selected installed QEMU executables and their native libraries:

```sh
sudo sh qemu-runners/cpp-runner/build-image.sh \
  "$PWD/build/dev/lab-qemu-runner" /var/tmp/gl7-runner-NEW \
  graphlab-m7/qemu-runner:qualification
```

The script records file hashes and the immutable image ID. Preserve its build context and an exported Docker image. It is a local qualification image, not a published multiarchitecture release. The initial ROM dependency failure is retained; the shared argument builder now disables unused NIC option ROMs because these guests boot pinned firmware/kernel/disk artifacts.

## Experimental switch isolation

`namespace-ovs` remains explicitly rejected with `unsupported_backend`. The `m7_isolation` probe creates two separate network namespaces and shows that the default OVS socket still exposes the same host database/daemon. It cleans up only its own namespaces and never restarts the shared daemon. This is a rejection test, not qualification of independent OVS instances. Separate databases, daemon supervision, namespace-specific datapaths, sustained unrelated traffic under targeted restarts, and cleanup ownership would all be required before exposing such a backend. M7's exit criterion permits rejecting an unproven isolation mode.

## Reproduce tests

Build with CMake, then install the capture and terminal worker binaries root-owned, mode 0755, as required by existing fixtures. Use a dedicated Linux Docker/OVS/systemd host with PPC/TCG and ARM64/KVM guest inputs. The updated minimal C++ guest init supports validated `graphlab.data0=IPv4/24` and `graphlab.mgmt0=IPv4/24` kernel parameters, bound data-interface UDP probes/replies, and serial receive counters. Existing guest examples keep their default addresses. Build new guest artifacts separately; do not overwrite preserved M4/M6 inputs.

```sh
sudo build/dev/m7_isolation
sudo build/dev/m7_linux "$PWD" PPC_INPUT ARM_INPUT APP_IMAGE_ID RUNNER_IMAGE_ID
sudo build/dev/m7_linux "$PWD" PPC_INPUT ARM_INPUT APP_IMAGE_ID RUNNER_IMAGE_ID --ownership
# Reconfigure/rebuild with GRAPHLAB_TEST_CHECKPOINTS=ON for the crash phase:
sudo build/dev/m7_linux "$PWD" PPC_INPUT ARM_INPUT APP_IMAGE_ID RUNNER_IMAGE_ID --crash
# Restore GRAPHLAB_TEST_CHECKPOINTS=OFF for normal use.
```

The normal fixture covers host/container × PPC/ARM × guest–Docker/guest–guest, required captures, independent packet delivery, direction-specific loss/restoration, telemetry, watchdog failure, repeated cleanup and an unrelated sentinel. The crash fixture targets attachment bridge creation, veth creation/move, both port attachments, both QoS creation boundaries, and runner container creation/start. Retained failed fixture directories can be recovered with `sudo build/dev/m7_linux --recover EXACT_CASE_DIRECTORY`; do not delete Docker or OVS resources globally.

Direct attachment ports carry owned `linux-noop` QoS rows so OVS does not reset externally managed fault queues ([OVS QoS schema](https://www.openvswitch.org/support/dist-docs/ovs-vswitchd.conf.db.5.html)). Fresh direct TAPs receive a verified priority parent; directional netem attaches beneath it. Unknown roots/children are rejected. Cleanup verifies bridge, port and QoS identities, refuses foreign ports, and removes orphan owned QoS rows. The additional `--ownership` fixture checks both guest–guest loss directions and a foreign-port cleanup refusal. Capture files retain the exact observation mapping; endpoint receive counters provide delivery evidence independently.

A container can bind QMP/serial sockets after an interrupted start's initial stop check. Recovery therefore performs a second scoped socket cleanup after Docker confirms container removal. The crash fixture includes this boundary and both QoS creation boundaries (nine checkpoints total).

Removing a direct-TAP fault restores an explicit, owned 1,000-packet FIFO child beneath the priority parent. It does not leave an empty priority band after deleting netem. The direction fixture repeats each fault direction twice and requires both loss and restored endpoint delivery, while retaining its measured counters.
