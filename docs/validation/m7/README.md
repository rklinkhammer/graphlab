# M7 validation

M7 implements direct guest attachment bridges and the optional C++ QEMU container runner described in [operations](../../m7-backends.md). Experimental namespace OVS remains rejected; the probe demonstrates that separate network namespaces alone still share the default OVS database and daemon. This is not a qualified independent-switch backend.

The selected runtime is the existing ARM64 Ubuntu Linux lab VM: GCC 13.3, Linux 6.8.0-139, Docker API 1.52, OVS 3.3.9 and QEMU 8.2.2. Guest tests cover PPC64LE/TCG and ARM64/KVM, both as host processes and through the pinned native container runner. Other host architectures and larger sustained-load envelopes are not qualified by these tests. M6's capacity and compatibility evidence remains historical and unchanged.

## Acceptance

- Eight combinations: host/container × PPC/ARM × guest–Docker/guest–guest. Each requires one capture for one logical edge, a separately inventoried IP-less attachment, actual endpoint delivery, valid telemetry, earliest numbered guest traffic in independently read PCAPNG, repeated scoped recovery and an unrelated sentinel preserved. Container cases additionally kill the terminal supervisor and require the independent runner watchdog to terminate QEMU within eight seconds.
- Ownership/fault cases: reversed Docker/guest endpoint order, real packet loss and restoration in both direct-link directions, independent guest receive counters for TAP direction, refusal to remove an attachment containing a foreign port, and successful recovery after the fixture explicitly removes that port.
- Nine SIGKILL checkpoints: attachment bridge; veth creation/move; both QoS creation boundaries; both completed port attachments; runner container creation/start. Recovery must succeed twice and leave no owned links, containers, worker units, sockets or OVS Bridge/Port/Interface/QoS rows. Unrelated sentinel bridges must survive.
- Isolation probe: distinct namespace inodes expose the same default OVS database. `namespace-ovs` must fail shared contract validation before mutation; no shared daemon restart is performed or described as an isolated restart.
- Portable CTest, ASan/UBSan, privileged transport, and existing M2–M5 Linux regressions cover the retained shared backend.

## Fixes exposed by integration

The first container attempt lacked an optional NIC boot ROM. The shared argument builder now disables unused network boot ROMs. An early guest receive check read only the first page of serial replay; the fixture now follows every replay cursor. Direct TAP faults initially encountered kernel default queues; a zero-length queue experiment dropped actual delivery and was rejected. The final implementation uses owned priority parents and `linux-noop` QoS rows, including explicit QoS cleanup. A malformed OVS map condition in cleanup was corrected with OVS string quoting. Repeated directional checks exposed a priority-band restoration defect; fault removal now restores an explicit owned FIFO, and serial counter reads require a complete line. A container-start crash exposed late-created sockets; cleanup now rechecks after confirmed container removal. Failed attempts are retained alongside passing evidence.

SDK source is now 1.2.0 for the additive contract changes; gate wire protocol remains 1.1. Frozen M6 SDKs/images and manifests are not overwritten. The runner image uses the common argument builder and shared contract/document library and contains no custom Python support.

## Evidence and reproduction

Large artifacts live in the Git-ignored `qualification/artifacts/linux-arm64-m7/` directory. Preserve that directory when transferring the workspace. It contains the final source, image/rootfs, guest inputs, raw per-case state/recordings, runtime identities, logs and SHA-256 inventory. Guest inputs contain disposable fixture credentials; keep the bundle private. Small test logs and the hash inventory are mirrored here. Hashes demonstrate artifact integrity, not a universal platform qualification.

Use the commands in [M7 operations](../../m7-backends.md). `validation.sh` records the staged build/test commands used for this runtime; `final-direct-validation.sh` repeats the affected paths after the final queue-restoration fix. Tests use 1,500-byte-MTU links and small numbered UDP probes, not a new sustained-capacity envelope. Runtime tests require Linux root, Docker, OVS, systemd, tun and KVM; macOS tests alone cannot qualify them. Crash injection is built with `GRAPHLAB_TEST_CHECKPOINTS=ON`; final normal builds restore it to OFF. Failed fixtures can be recovered only by their exact retained case path; recordings and immutable source/image archives are intentionally preserved.

## Recorded result

[Results](results.json): eight backend combinations, nine crash checkpoints, reversed endpoint/foreign-port checks, and four repeated guest–guest directional fault cycles passed. macOS CTest passed 18/18 and ASan/UBSan passed 17/17 (installed consumer excluded). Linux CTest passed 17 tests with the root-only transport check skipped unprivileged; its separate privileged invocation passed. Existing M2/M3/M4/M5 Linux suites passed, and the shared M5 fault suite was repeated after the final queue fix. SDK 1.2.0 independently built app-a and app-b from copied node-only source trees.

The [final cleanup audit](m7-cleanup-final.txt) passed, and [M6 integrity verification](m7-prior-m6-integrity.json) still reports its original manifest hash and qualified result. The runner's image binary hash matches the final host-built wrapper. `SHA256SUMS` seals the separate private artifact bundle; run `shasum -a 256 -c SHA256SUMS` from that directory to verify it.
