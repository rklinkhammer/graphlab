# QEMU guest templates

These are runnable starting points for future Graphlab examples. Custom guest support is shared C++23 in `common/init.cpp`; the host uses the common contract, lifecycle, recording and executor packages. No Python is required.

| Template | Guest | QEMU machine | Acceleration | Qualified host |
| --- | --- | --- | --- | --- |
| [ppc64le-tcg](ppc64le-tcg/topology.yaml) | PowerPC 64-bit little-endian Linux | `pseries-8.2` | Explicit TCG software emulation | ARM64 Linux |
| [arm64-kvm](arm64-kvm/topology.yaml) | ARM64 Linux | `virt-8.2` | KVM hardware virtualization | ARM64 Linux with `/dev/kvm` |

KVM requires a compatible host ISA. The PowerPC example uses TCG on the ARM64 lab host; selecting KVM does not accelerate a foreign ISA. Neither template silently falls back to another accelerator. Both use one vCPU, 512 MiB RAM, pinned firmware/kernel/initrd and an immutable 16 MiB raw base disk. Runs get private QCOW2 overlays. The examples boot their minimal userspace from the initrd; the disk is available as a template extension point.

## What is preserved

Each profile has a tracked `topology.yaml`, `artifacts.lock.json` and `SHA256SUMS`. The actual verified guest files are preserved **in this workspace** under its `artifacts/` directory: kernel, firmware, initrd, disk, matching modules archive, known-hosts file and private SSH client key. The pinned Dropbear source archive is in `common/artifacts/`. Binary artifacts and keys are intentionally Git-ignored: a fresh Git checkout alone does not contain these guests. Back up the artifact directories privately alongside the source if you need to move or retain these exact instances. The initrd itself contains the guest's private host key.

[inputs.lock.json](common/inputs.lock.json) records source provenance. The per-profile lock records actual content hashes, including firmware and SSH host identity. The Debian download URL is a moving source location, not a replacement for the preserved hash-verified files. QEMU machine versions are pinned; the tested QEMU package version is recorded separately.

## Validate and run a preserved profile

Portable validation and planning require only the normal C++ build:

```sh
cmake --preset dev
cmake --build --preset dev
example=qemu-guests/ppc64le-tcg # or qemu-guests/arm64-kvm
(cd "$example" && shasum -a 256 -c SHA256SUMS)
build/dev/lab validate "$example/topology.yaml" --lock "$example/artifacts.lock.json"
build/dev/lab plan "$example/topology.yaml" --lock "$example/artifacts.lock.json"
```

Actual execution requires Linux, systemd, QEMU, qemu-img, Docker (for the management network), OVS, libpcap, OpenSSH client and the installed root-owned `lab-agent`, `lab-terminal`, `lab-capture` binaries. Install the CMake build to a trusted root-owned binary directory. Use the separated agent/API accounts described in [M2](../docs/m2-executor.md).

On that Linux host, copy one profile **including its private artifacts** into your workspace and stage it before launching the agent:

```sh
example=qemu-guests/ppc64le-tcg
state=/var/lib/graphlab-example-ppc
sh qemu-guests/common/stage.sh "$example" "$state"
# Set the agent's --topologies to "$example", --lock to
# "$example/artifacts.lock.json", and --state to "$state".
# Use the usual private --socket and --allow-uid settings from M2.
```

Select the profile in the console and start it. Serial recording starts before guest release; readiness appears separately from process readiness. Open a recorded session for `guest` to use pinned-key SSH. The guest console supports `help`, `status`, `echo` and `exit`; it is a C++ fixture console, not a general Unix shell.

Run the two templates sequentially or in separate isolated Linux environments. Both intentionally use the same management subnet (`172.31.243.0/24`), workload credential name and guest address (`172.31.243.10`). A single agent admits one active run. Separate state roots prevent one profile's SSH credential from replacing the other's.

## Extend or rebuild

Copy a profile directory to start a new example, then change topology IDs, ports, workloads and addresses as needed. The initial graph has one guest, one OVS switch and one captured data edge. Topology validation/planning remains graph-driven: these shapes are examples, not hard-coded graph limits. This milestone attaches QEMU data NICs to declared OVS ports; direct guest-to-workload edges remain outside the qualified M4 backend.

The shared C++ init configures `eth0` as `10.233.17.2/24` and `eth1` as `172.31.243.10/24`. Dropbear binds only to `172.31.243.10:22`; update that binding as well when changing management addressing. It sends UDP probes to `10.233.17.1:49000`. The standalone template does not assign that receiver address; add a matching receiver workload for an application experiment. The acceptance test assigns it to its own OVS internal interface. Keep init configuration, topology addresses and SSH known-hosts identity consistent when deriving examples.

To rebuild on ARM64 Linux, use a fresh private output directory. Install native g++, the PowerPC cross compiler (`g++-powerpc64le-linux-gnu`), make, autoconf, cpio, gzip, bzip2 and OpenSSH tooling. Use a native `dropbearkey` for key generation; the ARM64 Dropbear build supplies it. Preserve kernel-matching virtio/failover modules for PowerPC; the qualified ARM kernel has the needed virtio drivers built in.

```sh
profile=arm64-kvm
out="$PWD/build/guest-$profile"
sh qemu-guests/common/build-ssh.sh "$profile" \
  qemu-guests/common/artifacts/dropbear-2025.88.tar.bz2 "$out"
sh qemu-guests/common/prepare-keys.sh "$out" "$out/dropbear-build/dropbearkey"
mkdir -p "$out/modules"
tar -xzf "qemu-guests/$profile/artifacts/modules.tar.gz" -C "$out/modules"
sh qemu-guests/common/build.sh "$profile" \
  "qemu-guests/$profile/artifacts/kernel" "qemu-guests/$profile/artifacts/firmware" \
  "$out/modules" "$out"
# For ppc64le-tcg, use that profile and the native ARM dropbearkey above.
```

Build SSH and keys **before** the initrd. `prepare-keys.sh` preserves existing keys; fresh derived examples should use a fresh output directory and new keys. Dropbear is pinned third-party SSH transport; all custom lab init/console behavior is C++. Copy the rebuilt kernel, firmware, initrd, disk, known_hosts and private client.key into your derived example's `artifacts/` directory, then regenerate its metadata:

```sh
build/dev/lab-guest-template arm64-kvm path/to/example/artifacts path/to/example
```

The generator hashes real inputs and validates its output through the shared contract package. It overwrites the generated topology/lock, so apply your graph-specific changes afterwards and update `artifactLock` with `lab hash` if the lock changes. Rebuilds are content-pinned after generation; byte-for-byte reproducibility across toolchain versions is not claimed.

See [M4 operations](../docs/m4-qemu-consoles.md) and [verification](../docs/validation/m4-verification.md).

M7 adds direct guest–Docker/guest–guest attachment examples and an optional container runner; see [M7 backends](../docs/m7-backends.md). The C++ minimal init accepts validated `graphlab.data0=IPv4/24` and `graphlab.mgmt0=IPv4/24` kernel parameters, binds its UDP fixture to the data NIC, and reports received packet counts over the recorded console. Rebuild into new artifact directories to preserve earlier template images.
