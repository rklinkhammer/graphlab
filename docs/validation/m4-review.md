# M4 verification review

**Remediated:** both findings below are fixed and covered by added Linux regressions. See [M4 remediation](m4-remediation.md). This review preserves the original failure evidence.

Reviewed 2026-09-22 against the current uncommitted M4 implementation on baseline `b8c69f2`. **M4 is not ready for acceptance:** two additional Linux checks fail despite the existing suites passing. No implementation remediation was applied during this review.

## Findings

### P1 — Closing a console can leave its Docker exec running without a recorder

`cpp/terminal/manager.cpp:107–112` returns when the transient worker unit is absent, before the Docker exec identity lookup and termination at line 114. A worker that exits successfully is normally unloaded by systemd. The same path is relevant to the worker's idle expiry: the worker closes its stream but does not own/terminate the Docker exec itself.

Reproduced with the existing real Docker console fixture: open a recorded shell, stop only its owned recorder with `systemctl stop`, wait for the transient unit to disappear, then call the normal terminal `close` operation. The API succeeds, but Docker exec inspection still reports `Running=true`. This is the shell leader itself, not the previously documented uncertainty about deep descendants. The run recovery operation subsequently cleans it up by removing the container.

[Evidence](m4-review-linux.txt): `recorder unit absent=1`, `exec running before close=true`, `exec running after successful close=1`, followed by successful run cleanup and the failed negative assertion. The existing acceptance test only uses `check(true, "scoped shell closed")` after the close call; it does not check the exec's state.

Required remediation: make exact owned-exec cleanup independent of worker-unit presence, and ensure recorder expiry/failure cannot leave an unrecorded shell running indefinitely. Verify the exec stops while its parent workload remains alive, including an already-exited recorder.

### P2 — Preserved guest SSH listens on the data network

`qemu-guests/common/init.cpp:63–65` starts Dropbear without an address-specific listener. It therefore accepts connections on the guest data interface as well as management. This conflicts with the plan's requirement that support services bind only to management/local control (`docs/option-d-console-plan.md:523`).

Reproduced on the preserved ARM64/KVM guest: the fixture's host-side data receiver queried `10.233.17.2:22` using `ssh-keyscan -T 3 -t ed25519`. It received the guest SSH host key. The configured management address is `172.31.243.10`. Authentication is still required for an SSH session; this finding establishes unintended service exposure, not an authentication bypass. The common init source is used by both templates; the negative runtime probe was run on ARM64.

[Evidence](m4-review-qemu.txt): `guest SSH accessible on data address=1`. The test then completed watchdog, abrupt supervisor shutdown, recovery, retained export and partial-recording checks before failing the management-only assertion.

Required remediation: bind the SSH listener to the declared management address, rebuild both initrds and update their content locks, and add a negative data-interface reachability check alongside the positive pinned-key management SSH test.

## Checks run

| Check | Result |
| --- | --- |
| Current macOS build/CTest | 11/11 passed; [log](m4-review-macos.txt) |
| Current macOS ASan/UBSan | 10/10 passed, installed consumer excluded; [log](m4-review-sanitizers.txt) |
| TypeScript/Vite build | Passed; existing bundle-size and dependency directive warnings remain |
| Existing browser regressions | 4/4 passed |
| Linux CTest | 10 passed, root-only transport test skipped; [log](m4-review-linux.txt) |
| Existing root Docker console suite | Passed; [log](m4-review-linux.txt) |
| Recorder-exit/exec-close negative test | **Failed**, after successful fixture cleanup |
| ARM64/KVM review run | Existing readiness, SSH, capture, replay, outage, shutdown and recovery checks passed; management-only SSH assertion **failed** |
| Preserved template checksums and validation | Both profiles passed; [log](m4-review-templates.txt) |
| Whitespace checks | `git diff --check` passed |

Linux-tested terminal, QEMU, engine, backend and HTTP source hashes match the workspace. The full PPC runtime, oversized-firmware failure test, M3 privileged suite and live Linux browser test were not rerun in this review; their earlier results remain implementation evidence, not fresh review results. No claim of a complete failure matrix is made.

## Reproduction

[This patch](m4-review-repro.patch) adds the two negative assertions to the existing explicit Linux acceptance programs. It targets the pre-remediation M4 sources; the current tests now include the fixed regressions. To reproduce the original failures, apply it in a disposable copy of those pre-remediation sources, rebuild `m4_console_linux` and `m4_linux`, then invoke them as documented in [M4 verification](m4-verification.md). The QEMU variant selects ARM64 and omits the unrelated oversized-firmware case. Both tests perform normal owned-run recovery before asserting the observed failure, so a failing negative assertion does not intentionally leave its fixture active.

For this review the variants were compiled as separate executables beside the original Linux binaries. The workspace's production and existing acceptance source files were not altered. The historical browser screenshot generated by regression testing was restored. All review-created workers, containers and bridges were cleaned up; see [cleanup](m4-review-cleanup.txt).
