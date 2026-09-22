# M3 implementation verification

**Follow-up review and remediation:** [Independent M3 verification](m3-review.md) reproduced two blocking defects despite passing existing suites. Both are now fixed: interrupted-stop coverage remains incomplete and worker disconnect no longer terminates the agent with SIGPIPE. See [M3 remediation](m3-remediation.md) for current regression results; the implementation evidence below is historical.

Verified on 2026-09-22. M3 implements capture-first Docker/OVS runs with C++ libpcap workers, PCAPNG artifacts, an all-edge release barrier, shared C++ node traffic leases, and browser artifact downloads. No Python support was added. QEMU remains M4 work.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| macOS build and CTest | 9/9 passed | [CTest](m3-macos-ctest.txt) |
| macOS ASan/UBSan | 8/8 passed (installed consumer excluded) | [Sanitizers](m3-macos-sanitizers.txt) |
| Linux build and CTest | 9/9 passed, including independent libpcap readback | [CTest](m3-linux-ctest.txt) |
| Linux privileged capture acceptance | Passed | [Runtime](m3-linux-runtime.txt) |
| Existing browser regressions | 4/4 passed | [Browser regressions](m3-browser-regression.txt) |
| Live capture browser workflow | Start, stop, verified download, resume, destroy passed | [Browser](m3-browser-live.txt), [screenshot](m3-console.png) |
| Actual API/agent process restarts | Worker identities survived; higher generation adopted without implicit traffic release | [Process restarts](m3-process-restart.txt) |
| M2 privileged regression | All 56 before/after mutation crash cases passed; unrelated sentinel preserved | [M2 regression](m3-m2-regression.txt) |
| Wireshark independent file inspection | 121 finalized PCAPNG files accepted by capinfos 4.2.2 | [capinfos](m3-capinfos.txt) |

The privileged suite verifies five-edge ring activation before release and captures the earliest application probes. It checks stop/resume epochs, rotation, packet-driven quota exhaustion, a killed capture worker, invalid filter admission, OS-enforced file-write denial, worker identity replay refusal, controller absence/adoption, direct Docker-to-Docker namespace capture, and an isolated zero-edge topology. Failed starts leave workload gates held. Partial files remain distinguishable from finalized artifacts.

Measured fixture observations: capture failure with the controller present led to quiescence in **2,073 ms**; controller absence plus capture failure led to held node gates in **9,963 ms**, within the test's 12-second target. External UDP probes confirmed that the expired node gate no longer echoed. These are measured observations, not worst-case scheduling bounds. [Measurements and image identities](m3-runtime-measurements.txt).

Portable regression tests also verify that restart preserves known incomplete coverage and that stopped runs retain closed capture state without adopting exited workers. Required-capture admission rejects workloads advertising restart-required quiescence before creating a run.

## Environment and reproduction

Host: macOS ARM64, AppleClang 21, CMake 4.4.3, Boost 1.92, OpenSSL 3.6.4, SQLite 3.54. Linux: Ubuntu 24.04 ARM64, kernel 6.8, GCC 13.3, CMake 3.28.3, OpenSSL 3.0.13, SQLite 3.45.1, libpcap 1.10.4, Docker 29.1.3, OVS 3.3.9, systemd 255.

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --preset dev
npm run build --prefix console/web
npm test --prefix console/web -- tests/console.spec.ts
```

The separate root/Linux acceptance executable requires Docker, OVS, systemd, libpcap, a trusted root-owned `lab-capture` beside `lab-agent`, and independently built app-a/app-b images with traffic-lease protocol labels:

```sh
sudo build/dev/m3_linux "$PWD" "$PWD/build/dev/m2_tests" \
  sha256:APP_A_IMAGE_ID sha256:APP_B_IMAGE_ID
```

See [capture operations](../m3-captures.md) for build requirements and policy. The browser integration test requires the dedicated Linux fixture and authenticated API on port 18089; it is explicitly gated by `GRAPHLAB_M3_LIVE=1`. A skipped live test is not a passed acceptance test.

Development packages and Wireshark tools were extracted into private VM directories; system packages were not upgraded. Test-created active resources were cleaned up. Captures, diagnostic state, and image caches remain as evidence.

## Qualification limits

The shared C++ Docker fixtures qualify the current lease profile. Arbitrary third-party applications, real-time deadlines, QEMU, reboot recovery, Linux x86, a broader compiler matrix, scale, and exhaustive fault interleavings are not qualified here. The live browser/process tests and M2 regression preceded the final capture-only restart/admission refinements; those refinements are covered by the final portable and Linux capture reruns.

Offload state and per-packet direction remain explicitly unknown. Drop counters are reported where libpcap supplies them; unknown counters remain null. Readiness and bounded final draining do not promise zero packet loss. Partial-file inspection preserves bytes and reports structural prefixes; it does not repair arbitrary PCAPNG input. Artifact exports currently cover closed segments only.

An earlier concurrent verification run hit an M1 RPC timing assertion/timeout, and another sanitizer run timed out after the M3 success message. Sequential reruns passed. No root cause was established, so these results do not establish stress-load qualification.
