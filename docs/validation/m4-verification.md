# M4 verification

**Follow-up review:** [M4 verification review](m4-review.md) reproduced a Docker console cleanup defect and guest SSH exposure on the data network. Both findings are now fixed; the [post-remediation re-verification](m4-reverification.md) passed the current portable, Docker, two-profile QEMU and live browser checks. The evidence below records the initial implementation.

Verified on 2026-09-22. M4 implements host QEMU execution and recorded serial, pinned-key SSH and Docker PTY consoles in C++23. PowerPC/TCG and ARM64/KVM guest instances are preserved locally as [templates](../../qemu-guests/README.md), with tracked topology/lock/checksum metadata and Git-ignored private binary artifacts.

## Results

| Check | Result | Evidence |
| --- | --- | --- |
| macOS C++ build and CTest | 11/11 passed | [CTest](m4-macos-ctest.txt) |
| macOS ASan/UBSan | 10/10 passed; installed-package consumer excluded | [Sanitizers](m4-sanitizers.txt) |
| Template hashes, validation and dry-run plans | Both profiles passed | [Templates](m4-templates.txt) |
| Linux CTest | 10 passed; privileged transport test skipped as non-root | [Linux regression](m4-linux-regression.txt) |
| Linux root M4 acceptance | PowerPC/TCG, ARM64/KVM and Docker suites passed | [QEMU and console runtime](m4-linux-runtime.txt) |
| Linux root M3 capture regression | Passed, including privileged transport acceptance | [Linux regression](m4-linux-regression.txt) |
| Browser regression | 4/4 passed | [Browser regression](m4-browser-regression.txt) |
| Live Linux browser console | 1/1 passed, including replay, download and authorization checks | [Live browser](m4-browser-live.txt), [screenshot](m4-console.png) |

The portable suite exercises single-writer ownership, takeover fencing, expired renewal, exact opaque replay including NUL bytes, sequence/offset cursors, symlink rejection, closed publication, partial tails and quota exhaustion.

The root QEMU suite boots `pseries-8.2` with PPC64LE Linux using TCG and `virt-8.2` with ARM64 Linux using KVM. It tests capture-first release, distinct guest readiness, pinned-key SSH authentication, PTY input/resize, serial replay, controller-outage survival, watchdog pause, actual guest probe traffic cessation, higher-generation reconciliation, retained recording export and oversized-firmware launch-failure cleanup. The KVM case also abruptly kills the owned supervisor before recovery and expects a preserved partial recording. Serial coverage is explicitly from attachment; first-byte capture is not claimed.

The Docker suite checks scoped exec attachment, resize with `stty`, input/output, default omission of exact input records, writer conflict/takeover, recording through controller absence, adoption and cleanup. The live browser test checks unauthenticated WebSocket refusal, authenticated binary replay, writer input, reconnect, run destruction, a checksum-verified `.glterm` download and invalid-CSRF refusal on WebSocket control messages.

## Environment and reproduction

macOS ARM64: AppleClang 21, CMake 4.4.3, Boost 1.92, OpenSSL 3.6.4. Linux lab VM: Ubuntu 24.04 ARM64, kernel 6.8.0-134, GCC 13, CMake 3.28, Docker 29.1.3, OVS 3.3.9, systemd 255 and libpcap 1.10.4. QEMU 8.2.2 (`1:8.2.2+ds-0ubuntu1.18`); firmware and kernel inputs are recorded in the [guest provenance](../../qemu-guests/common/inputs.lock.json) and profile locks. PowerPC QEMU and cross-build dependencies were installed in the lab VM. KVM is exercised only for the native ARM64 guest.

```sh
cmake --preset dev
cmake --build --preset dev -j6
ctest --preset dev
npm run build --prefix console/web
npm test --prefix console/web -- tests/console.spec.ts
```

For sanitizers, configure a separate Debug build with `-fsanitize=address,undefined -fno-omit-frame-pointer` in `CMAKE_CXX_FLAGS` and run CTest excluding `installed_package`. Linux root acceptance additionally requires the M3 Docker fixture images, systemd, OVS, QEMU, `/dev/kvm`, and root-owned `lab-terminal`/`lab-capture` beside the test executables. No other Graphlab agent may hold the executor lock:

```sh
sudo build/dev/m4_linux "$PWD" \
  "$PWD/qemu-guests/ppc64le-tcg/artifacts" "$PWD/qemu-guests/arm64-kvm/artifacts"
sudo build/dev/m4_console_linux "$PWD" sha256:APP_A_IMAGE_ID
sudo build/dev/m3_linux "$PWD" "$PWD/build/dev/m2_tests" \
  sha256:APP_A_IMAGE_ID sha256:APP_B_IMAGE_ID
```

The live browser test is gated by `GRAPHLAB_M4_LIVE=1`. It requires dedicated Linux services with an M2 development fixture, API forwarding to local port 18089, and a private `build/m4-live-password` file. A skipped live test is not acceptance evidence. Tests create resources only through owned fixture state. Diagnostic state and recordings remain in private lab-VM directories; guest templates remain in the workspace. Temporary browser API/agent services and forwarding were stopped, and their credentials removed after verification. See [cleanup](m4-cleanup.txt).

## Limits and observations

A deliberately malformed small kernel or firmware blob can still be accepted by QEMU as a raw boot payload. This does not prove guest readiness. The deterministic launch-failure case therefore uses firmware larger than the machine's firmware storage. There is no universal guest boot-health policy in this milestone. Faulted launch detection uses bounded retries and can take minutes; a short startup-failure deadline is not qualified.

The watchdog test waits eleven seconds after controller loss, verifies QMP reports paused, then verifies captured application probe counts stop advancing after a drain interval. This establishes observed cessation for these fixtures, not a hard real-time bound under arbitrary load. Required captures cover declared data links; management traffic is outside that scope.

The M4 scope does not qualify Linux x86, other QEMU versions, reboot recovery, exhaustive crash interleavings, wrong-key injection, hostile terminal fuzzing, deep Docker exec descendant cleanup, global console/capture quota rebalancing, scale, or every third-party guest. An interruption between TAP creation and ownership tagging may leave an ambiguous resource for manual recovery; cleanup does not delete an unverified TAP. Existing M2 crash-matrix evidence remains historical and was not rerun for every new M4 side effect. Recording pause and retention/eviction policy remain unimplemented.
