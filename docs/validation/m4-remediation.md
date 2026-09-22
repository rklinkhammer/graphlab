# M4 remediation

The two findings in the [M4 review](m4-review.md) are fixed. This is targeted remediation of the current uncommitted M4 implementation; the review's original failure evidence is retained.

## Changes

Docker cleanup no longer treats an absent recorder unit as proof that its exec has exited. It uses the journaled exec/container identity, obtains a pidfd, rechecks identity, terminates that exec leader and waits for Docker to report it stopped. Natural-exit races are handled without signalling a reused PID. Repeated close remains safe and the parent workload stays running.

New Docker recorder units also have a systemd `ExecStopPost` hook calling `lab-terminal --cleanup`. This runs after normal stop or worker death, independently of the agent. The helper verifies the private root-owned config and boot identity, then performs the same scoped cleanup. Its `+` command prefix deliberately gives this short-lived cleanup helper the filesystem access required to contact Docker; it does not give the recording process access to the Docker socket. A file lock serializes exec creation/attachment against cleanup. Session-open failure attempts cleanup and records recovery-required state if cleanup itself fails. Existing units from the previous implementation must be closed/recreated to gain the exit hook; explicit close works with their retained descriptors.

The common guest init binds Dropbear to `172.31.243.10:22`, the templates' management address. Both PowerPC/TCG and ARM64/KVM initrds were rebuilt, copied back into their preserved workspace artifact directories, and their locks/checksum manifests regenerated. Existing SSH keys were preserved. Template validation and file checksum verification pass.

## Regression evidence

- [Linux portable and Docker regression](m4-remediation-linux.txt): ordinary recorder stop and forced worker SIGKILL both terminate the exec before explicit close. An additional independently created, journaled orphan exec exercises close with no recorder unit. The test checks actual Docker `Running` state, repeated close and preservation of the parent workload, then recovers the fixture.
- [PowerPC/TCG and ARM64/KVM](m4-remediation-qemu.txt): pinned-key management SSH succeeds; data-address SSH keyscan cannot retrieve a host key on either guest. Existing readiness, resize, replay, traffic-watchdog, retained artifact and invalid-firmware cleanup checks pass. The ARM64 abrupt-shutdown case still preserves partial recording evidence.
- [macOS CTest](m4-remediation-macos.txt): 11/11 passed.
- [ASan/UBSan](m4-remediation-sanitizers.txt): 10/10 passed, installed-package consumer excluded.
- [Templates](m4-remediation-templates.txt): preserved artifact checksums and both generated topology/lock pairs validate.
- [Cleanup](m4-remediation-cleanup.txt): no active Graphlab workers, owned running containers or OVS bridges left by the tests.

The strengthened tests are in `tests/terminal/linux.cpp` and `tests/qemu/linux.cpp`. Invoke the Docker test normally and again with `sudo env GRAPHLAB_TEST_KILL_RECORDER=1`; invoke the QEMU test with both rebuilt guest artifact directories as documented in [verification](m4-verification.md).

The final natural-exit race handling was followed by fresh portable/sanitizer and both Docker test runs. The two-profile QEMU run preceded that Docker-only refinement. Browser/UI code was unchanged; the earlier browser and M3 privileged results were not rerun for this remediation. Linux CTest's privileged transport test skips under the unprivileged runner.

This does not add deep Docker exec descendant supervision, host-reboot qualification, or guaranteed cleanup while Docker/systemd is unavailable. Such failures remain explicit recovery conditions. The normal idle-expiry path shares the tested recorder-exit hook, but the test does not wait thirty minutes to exercise the wall-clock idle threshold.
