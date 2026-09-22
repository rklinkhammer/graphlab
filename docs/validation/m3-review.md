# M3 independent verification — 2026-09-22

**Remediated:** both findings below were subsequently fixed and regression-tested. See [M3 remediation](m3-remediation.md) for current results. This report preserves the original failure evidence.

**Result: not passed.** The existing suites pass, but two additional regression probes reproduce M3 correctness failures. Production implementation files were not changed during this review.

## Findings

### P1 — interrupted captures can be reported as cleanly closed

`cpp/runtime/engine.cpp:610–614` discards the observations returned by `capture_control(run, "stop")`. The final state assignment at lines 635–639 then reports `closed`, unless the run was already marked `closed-incomplete`. The Linux manager legitimately returns an `interrupted` observation for a dead worker; it does not always throw.

Consequently, a worker failure followed by a stop before the periodic monitor detects the failure completes successfully and reports clean coverage. This violates the M3 requirement to expose capture gaps. It also leaves the old capture observations in the journal.

A standalone probe linked against the current runtime supplies an interrupted stop result through the backend interface. Actual output:

```json
{"captureCoverage":"closed","jobState":"succeeded","runState":"stopped","stopWorkerResult":"interrupted"}
```

[Probe source](m3-review/interrupted-stop.cpp), [output](m3-review/interrupted-stop.txt). This is a targeted Engine regression probe, not a claim that this exact race was injected into the live Docker suite.

Remediation: persist and evaluate final worker observations in stop, preserve incomplete coverage monotonically, and add a regression for a worker that dies between the last healthy check and stop. Also cover repeated cleanup: `cleanup()` at line 478 recognizes `incomplete` but not an existing `closed-incomplete`, allowing a later all-closed result to erase an earlier gap.

### P1 — a disconnected worker can terminate the agent with SIGPIPE

`cpp/capture/manager.cpp:72` calls `send(..., 0)` on the worker stream. The agent installs no SIGPIPE suppression. If the worker closes the connection between connect and send, the default signal action terminates the caller instead of raising the intended recoverable `worker_send` failure. The worker's own SIGPIPE handling does not protect the separate agent process.

A standalone probe repeatedly connects the actual `capture::worker_call` implementation to a local Unix socket whose peer immediately shuts down. The calling child exits on **signal 13 (SIGPIPE)** on both macOS and the ARM64 Linux VM; the Linux test runs as root, satisfying the worker peer check. [Probe source](m3-review/disconnected-worker.cpp), [Linux output](m3-review/disconnected-worker-linux.txt).

Remediation: suppress SIGPIPE appropriately for supported platforms, treat a failed/partial write as a transport failure, and register a disconnected-peer regression. Node leases still bound traffic in the qualified fixture, but they do not prevent the controller crash or replace explicit failure accounting.

## Fresh checks

| Check | Result |
| --- | --- |
| macOS configure/build/CTest | [9/9 passed](m3-review/macos.txt) |
| macOS ASan/UBSan, installed consumer excluded | [8/8 passed](m3-review/sanitizers.txt) |
| Linux build/CTest and privileged capture suite | [9/9 and live suite passed](m3-review/linux.txt) |
| TypeScript/Vite production build | Passed; dependency directive warnings only |
| Existing browser regressions | [4/4 passed](m3-review/browser.txt) |
| Independent capinfos readback | [126 finalized files accepted](m3-review/capinfos.txt) |
| Targeted interrupted-stop probe | Failed: false `closed` coverage |
| Targeted disconnected-worker probe | Failed: caller terminated by SIGPIPE |
| `git diff --check` | Passed |

The fresh live suite retained evidence under `/tmp/gl3-eEW0Uz`. It measured controller-present quiescence at **2,068 ms** and lease-enforced quiescence during controller absence at **9,923 ms**, below its 12-second target. [Measurements](m3-review/measurements.txt). No running containers, OVS bridges, or active capture units remained afterward. Retained files/images are intentional evidence.

The live browser workflow, actual API/agent process-restart experiment, and full 56-case M2 crash matrix were reviewed from the prior implementation evidence; they were not rerun in this verification pass. Reboot, broader ISA/compiler coverage, scale, exhaustive failure interleavings, and arbitrary third-party workload lease guarantees remain unqualified. QEMU is M4 work.

## Reproducing the additional probes

Build each source as a standalone C++23 executable using the same include paths and runtime link dependencies as `m3_tests` (inspect `ninja -C build/dev -t commands m3_tests`). They are deliberately retained as verification evidence, not registered as passing CTest tests.

Run the interrupted-stop probe with the source root and the fixture generator:

```sh
/path/to/interrupted-stop "$PWD" "$PWD/build/dev/m2_tests"
```

Run the disconnected-worker probe without arguments; on Linux, run it as root so the local socket peer satisfies the existing UID check. Both probes return nonzero when reproducing their failure. They create only temporary private test state/socket paths and remove them before exiting. Neither creates Docker or networking resources.
