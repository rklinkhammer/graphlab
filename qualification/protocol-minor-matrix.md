# T19 matrix declared before compatibility execution

These are immutable local qualification releases, not claims of published upstream versions. Freeze the installed C++ SDK/source at 1.0.0, the existing gate wire behavior as 1.0, and a controller fixture linked to the pre-change runtime. Then build SDK 1.1.0, whose additive `protocolMinor: 1` field preserves the v1 commands and response fields. An absent minor means 0. Node builds select an exact SDK version; SDK hashes and image IDs enter their artifact locks.

| app-a gate | app-b gate | Frozen legacy controller | Current controller |
|---|---|---|---|
| 1.0 | 1.0 | Start, bidirectional probe, quiesce, cleanup | Same |
| 1.0 | 1.1 | Same | Same |
| 1.1 | 1.0 | Same | Same |
| 1.1 | 1.1 | Same | Same |

Every application/version build gets a separate node-only source directory and an installed SDK. Neither uses sibling application or support sources. Archive SDKs, common/node sources, binaries, build logs, immutable base identity and Docker images.

Negative cases: both controller fixtures reject an unsupported workload-contract major at validation; the current controller rejects an actual gate wire major 2 before sending release to **any** node, even if an image label falsely claims v1. Unknown/minor/malformed required-feature handling is also covered by portable contract tests. The legacy controller predates that additional wire-response check; it is retained as historical compatibility evidence, not offered as a secure deployment choice.
