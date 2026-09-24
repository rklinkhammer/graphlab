# Increment E verification — 24 September 2026

Disposition: **no blocking implementation defect found within the documented GLM1 Docker fixture scope**. This is not full GraphX parity or completion of increment F. Production code was not changed during verification.

Reviewed increment E requirements against the reporter/wire schema, SDK lifecycle instrumentation, independent collector, SQLite transactions/retention, runtime/API scope checks, capture correlation and message-history UI. Reports remain separate from packet records and aggregate metrics. Captures are read from finalized manifest segments with identity/size/SHA-256 checks; no new capture process or tuple/time correlation is introduced. Legacy capabilities remain optional.

## Fresh checks

| Check | Result / evidence |
|---|---|
| Native build | `cmake --build --preset dev` passed. [Log](messages-verification-build.txt). |
| Native regression | `ctest --preset dev`: **25/25 passed**, 32.38 seconds. [Log](messages-verification-native.txt). |
| Console build | `npm run build --prefix console/web` passed, with existing Vite notices. [Log](messages-verification-console.txt). |
| Default browser suite | `npm test --prefix console/web`: **27 passed, 15 environment-gated skips**. [Log](messages-verification-browser.txt). The live message test was separately enabled below. |
| Dedicated Linux native and filesystem exhaustion | Existing qualified Linux `message_tests` passed, including SQLite SIGKILL transaction recovery and slow-reporter read isolation. Fresh 8 MiB temporary tmpfs ENOSPC preserved committed events and accepted retry after freeing space. [Log](messages-verification-linux.txt). |
| Real endpoint/capture/API/browser | `GRAPHLAB_MESSAGES_LIVE=1 npm test --prefix console/web -- tests/messages-live.spec.ts`: **passed**. [Log](messages-verification-live.txt), [reviewed screenshot](messages-verification-live.png). |
| Cleanup | Task services inactive; no remaining running containers, OVS bridges, capture or terminal workers. Temporary ENOSPC filesystem unmounted by its trap. [Log](messages-verification-cleanup.txt). |

The live run was `6ccb1890-bb78-4f13-8884-1b994cc045b9`. Source-send and target-receive reports contained message `fd8d07efed6d49d32072f7e2256b8bf3000000000000000a`. Independent PCAPNG parsing verified artifact `06bb6ff39329d5df01ae7ab3-0`, packet index **25**, block offset **2992**, SHA-256 `1dc7032218f5b4b49d18fc8f62a496cb86efbdb5de696de262d683b8c33f3466`. Selected edge `b-s` was exact; the retained union contained **three** occurrences and was ambiguous. Browser edge selection, session/CSRF/Origin rejection, wrong-node correlation rejection, agent restart and retained correlation after destruction passed.

## Environment and source comparison

Discovered existing task services inactive before starting them. Reused the isolated source/build `/tmp/graphlab-messages-20260924`, state `/var/tmp/gl6-messages-20260924`, port 18099 and pinned fixtures from [environment identities](messages-environment.txt). Started only `graphlab-messages-agent` (root, allow UID 501) and `graphlab-messages-api` (UID 501); no unrelated services were changed. The live test destroyed its run before service cleanup.

[Fresh source/asset comparison](messages-verification-source.json): backend implementation files, native message test, SDK lifecycle implementation and all deployed console assets are byte-identical. Two headers differ only in formatting, namespace-end comments and names of declaration parameters omitted locally: `message_observation.hpp` and `message_history.hpp`. Their implementation/type semantics match by source inspection. This verification reused the existing compiled Linux binaries/images, rather than claiming a new rebuild of byte-identical headers. Fresh local compilation and the installed-SDK consumer regression passed.

## Remaining qualification limits

- Existing tests exercise global event pruning but do not independently prove every configured ceiling: the 4 MiB body boundary, 256-source exhaustion, 24-hour expiry, SQLite page ceiling and each correlation scan/match limit lack dedicated boundary cases in the message test. These remain qualification gaps for F; code inspection is not represented as runtime evidence.
- The slow-reporter test covers history read-lock isolation, not sustained client backpressure or a whole-agent saturation benchmark. SIGKILL interrupts a SQLite transaction, not a complete host/power failure.
- The browser tests cover frozen pages, exact/ambiguous/unavailable presentation and real edge selection. Dedicated message-panel late-response, session-expiry and narrow-layout qualification is not newly established by those tests.
- No new QEMU message reporter, arbitrary protocol/encrypted payload correlation, TCP reassembly, application-edge inference, delivery/loss accounting or synchronized latency claim. Reporter-ring omissions are expected and visible; retained-scope uniqueness is not proof of delivery.

The limits in [the contract](../message-observations.md) and earlier A–D reports remain applicable. Proceed to F only with these qualifications visible.
