# M4 re-verification after remediation

Re-verified on 2026-09-22 against the current uncommitted M4 workspace. This report follows the [original review](m4-review.md) and [remediation](m4-remediation.md). No production code was changed during this verification.

The reviewed Linux terminal, QEMU, engine, backend, HTTP, regression-test and guest-init source hashes matched the workspace before execution. Both Linux guest initrd hashes also matched the locally preserved template artifacts. The scope is the supplied PowerPC/TCG and ARM64/KVM profiles on the ARM64 Ubuntu lab VM, not a general cross-platform qualification.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| macOS CMake build and CTest | 11/11 passed | [CTest](m4-reverification-macos.txt) |
| macOS ASan/UBSan | 10/10 passed; installed consumer excluded | [Sanitizers](m4-reverification-sanitizers.txt) |
| Linux CTest | 10 passed, privileged transport test skipped under the non-root runner | [Linux](m4-reverification-linux.txt) |
| Docker console regressions | Normal stop and SIGKILL variants passed | [Linux](m4-reverification-linux.txt) |
| PowerPC/TCG and ARM64/KVM | Both passed | [Linux](m4-reverification-linux.txt) |
| TypeScript/Vite build and browser regressions | Build and 4/4 tests passed | Existing Vite dependency-directive and bundle-size warnings remain |
| Live browser console | 1/1 passed | [Browser](m4-reverification-browser.txt) |
| Both preserved templates | Checksums, validation and dry-run plans passed | [Templates](m4-reverification-templates.txt) |
| Whitespace checks | `git diff --check` passed | |

The Docker regression checks actual exec termination before explicit close after either normal recorder stop or SIGKILL. It separately creates a journaled orphan exec, confirms it is running, closes it with no recorder unit present, repeats close, and verifies that the parent workload remains alive. These directly cover the original P1 finding.

Both guest profiles verify positive pinned-key SSH on management and a negative SSH probe on the data interface, covering the original P2 finding. They also exercise capture-first startup, guest readiness distinct from process startup, PTY input/resize, serial replay, controller-outage watchdog pause, cessation of captured guest probes, recovery and closed recording export. Oversized-firmware launch failure cleans up. The KVM case additionally checks recovery after abrupt supervisor death and preservation of a partial serial recording.

The live browser workflow checks unauthenticated WebSocket refusal, writer input, binary replay, reconnect, shell close, run destruction, a checksum-verified terminal recording download and invalid-CSRF refusal. The screenshot from this run is [retained separately](m4-reverification-console.png).

## Boundaries

The original two defects are covered by live regression tests. No additional blocking finding was identified in the reviewed paths. Earlier limits still apply: no Linux x86 or host-reboot qualification, exhaustive crash/race matrix, deep Docker exec descendant supervision, or guaranteed cleanup while Docker/systemd is unavailable. The thirty-minute idle threshold was not waited out; its shared exit hook is exercised by the normal-stop test. The privileged M3 capture suite and M2 crash matrix were not rerun in this pass; their earlier evidence remains historical. Serial coverage remains explicitly from attachment, without a first-byte guarantee.

Tests use owned fixture resources and restore the historical screenshots changed by browser tests. Temporary API/agent services, forwarding and credentials are removed at completion; [cleanup evidence](m4-reverification-cleanup.txt) records the final worker/container/bridge inventory. Preserved guest binaries and private keys remain in their Git-ignored workspace artifact directories.
