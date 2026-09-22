# M3 remediation — 2026-09-22

The two blocking defects from the [M3 review](m3-review.md) are fixed, with registered regression tests and Linux runtime coverage.

## Changes

- `Engine::close_captures` now handles finalization for both stop and cleanup. It persists final worker observations and marks interrupted workers `closed-incomplete`. Later closed observations cannot erase an existing `incomplete` or `closed-incomplete` result within that capture epoch. A successful stop means the workload is held and finalization is accounted for; it does not imply uninterrupted recording.
- Worker control writes use socket-local SIGPIPE suppression: `SO_NOSIGPIPE` on macOS and `MSG_NOSIGNAL` on Linux. Failed or partial writes remain transport failures. The agent's process-wide signal disposition is unchanged.
- Portable tests cover interrupted stop, saved observations, repeated stop, repeated destroy, and preservation of known coverage gaps. The new `m3_transport` regression restores the default SIGPIPE disposition in a child and performs 10,000 calls against a disconnecting peer, requiring handled write failures and normal process exit.
- The privileged Linux suite runs the transport regression with a root peer. It also kills an owned capture worker immediately before stop finalization, then verifies incomplete coverage, durable interrupted observations, held workload gates, and repeated cleanup.

## Validation

| Check | Result |
| --- | --- |
| macOS build and CTest | [10/10 passed](m3-remediation/macos.txt) |
| macOS ASan/UBSan, installed consumer excluded | [9/9 passed](m3-remediation/sanitizers.txt) |
| Linux build and unprivileged CTest | [9 passed, 1 explicitly skipped](m3-remediation/linux.txt) |
| Linux root transport regression and capture suite | [Passed](m3-remediation/linux.txt) |
| Original interrupted-stop reproducer | [Reports `closed-incomplete`](m3-remediation/interrupted-stop.txt) |
| Formatting and `git diff --check` | Passed |

The Linux CTest skip is intentional: the real worker protocol requires a UID-0 socket peer. The same transport executable is explicitly run as root by `m3_linux`, so that acceptance gate is exercised rather than waived. The macOS test reported 9,921 handled write failures and `caller_signal=0`.

Linux runtime evidence is retained at `/tmp/gl3-phVY0f`. After completion, no running containers, OVS bridges, or active capture units remained. Capture files and image caches remain as intentional evidence.

Reproduction uses the normal `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev` commands. Run the existing root `m3_linux` suite with the independently built app-a/app-b image IDs for privileged checks; it now also requires the sibling `m3_transport` test executable, built automatically through its CMake dependency.

This remediation changes no browser code. Prior browser and M2 full crash-matrix evidence remains historical; the M2 journal tests and complete M3 capture runtime suite were rerun. Reboot, broader architectures, scale, exhaustive scheduling races, and arbitrary third-party workload guarantees remain outside the qualified profile documented in the [capture guide](../m3-captures.md).
