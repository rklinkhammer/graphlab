# Increment B re-verification — 23 September 2026

**Passed within the documented increment B scope.** The prior P2 rejected-command recovery defect no longer reproduces in the regression and real API rejection checks. No additional blocking defect was identified in the reviewed paths. No implementation changes were made during this pass.

## Fresh evidence

- Native build succeeded; **23/23 CTest cases passed** (21.12 seconds). [Log](source-control-reverification-native.txt).
- Console production build succeeded with existing Vite notices. Default browser suite: **21 passed, 12 opt-in live cases skipped** (54.2 seconds). Coverage includes 409/429 rejection recovery with a fresh instance/epoch and request ID, persistent command errors across polling, server/network uncertainty with identical-request retry, and selection fencing.
- Dedicated Linux native source-control tests passed, including ownership, duplicates, pending-source exclusion, timeout, invalid acknowledgement, both 256-record ceilings and interrupted-journal reconciliation. [Log](source-control-reverification-linux.txt).
- SHA-256 comparisons matched inspected backend/workload sources, native tests, the source-controls component, API error/client implementation, live test and deployed HTML/JavaScript assets. [Exact comparisons](source-control-reverification-source.txt).
- **Fresh real Linux API/browser test passed** (18.1 seconds). The real API rejected a deliberately invalid epoch; the UI retained a failed outcome, refreshed capability and accepted a fresh pause. Alpha stayed at 12 while beta advanced 17→28. The independent checksum-verified PCAPNG check found zero alpha and eleven beta datagrams strictly inside the pause interval, with unchanged capture processes. Receiving, subsequent generation resume, authoritative run quiescence, authorization/identity negatives and restart retention also passed. [Log and packet references](source-control-reverification-live.txt), [panel](source-control-reverification-live.png).
- All task fixture runs were destroyed; services stopped; running Docker and OVS inventories empty. [Cleanup](source-control-reverification-cleanup.txt). `git diff --check` passed.

The live run used `/tmp/graphlab-source-20260923`, `/var/tmp/gl6-source-20260923`, and port 18096. Existing Docker-only, command-capacity and uncertain-outcome limitations remain. This pass does not add QEMU, power-loss, actual journal exhaustion or application-message correlation qualification. Increment C remains next.
