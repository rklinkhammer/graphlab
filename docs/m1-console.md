# M1 read-only console

M1 adds the C++23 `lab-agent` and `lab-api` services and a React/TypeScript console. The agent loads validated, immutable topology revisions; API and CLI reads use its versioned Unix-socket protocol. The shared LabSupport package remains independently consumable. No Python is required.

## Build and run

The C++ build additionally requires **Boost 1.92.0 headers and CMake package configuration**. Only header-based Beast/Asio facilities are used. The browser build uses Node/npm; exact dependencies and integrity hashes are in `console/web/package-lock.json`. Node 26.8.1 was used for validation.

```sh
cmake --preset dev
cmake --build --preset dev
npm ci --prefix console/web
npm run build --prefix console/web
ctest --preset dev
```

Run the following from the repository root, as an unprivileged user. Both services use the same UID in the M1 development profile. `lab-api` refuses root execution. Use a dedicated account without Docker-group membership for a persistent deployment.

```sh
mkdir -m 700 build/m1-runtime
build/dev/lab-api init-auth build/m1-runtime/auth.json
```

`init-auth` prints a newly generated operator credential once. Save it for the login form. The file contains only a salted PBKDF2-HMAC-SHA256 verifier (210,000 iterations), is created with mode 0600, and cannot overwrite an existing file.

In one terminal:

```sh
build/dev/lab-agent \
  --socket "$PWD/build/m1-runtime/agent.sock" \
  --topologies "$PWD/topologies" \
  --lock "$PWD/topologies/artifacts.lock.json"
```

In a second terminal:

```sh
build/dev/lab-api \
  --socket "$PWD/build/m1-runtime/agent.sock" \
  --auth "$PWD/build/m1-runtime/auth.json" \
  --assets "$PWD/console/web/dist" \
  --port 8088
```

Open **http://127.0.0.1:8088** and enter the generated credential. Use that exact host: the service enforces Host and Origin, and does not accept `localhost` as an alias. When running inside Linux, forward the same port with `ssh -L 127.0.0.1:8088:127.0.0.1:8088 …` and use the same URL. Port translation requires matching the configured browser authority and is not supported by these launch flags.

The API binds only IPv4 loopback. HTTP is the explicit local/tunneled profile; cookies are HttpOnly, SameSite=Strict, and path `/`. They intentionally omit Secure for loopback HTTP. Remote HTTPS deployment is not implemented. Sessions expire after 30 minutes, are revoked at logout, and disappear on API restart. Logout requires its session's CSRF token. Login requires a same-origin request and is rate limited; at most 32 sessions are retained.

Stop each service with Ctrl-C. Orderly agent shutdown removes its socket. After an unclean termination, confirm the old agent is stopped before removing its stale socket; startup refuses to replace an existing path. Restart the agent to load edited files: the catalog is an immutable startup snapshot.

## Read surface

| Route | Result |
|---|---|
| `POST /api/v1/login` | JSON `{password: "…"}` → session cookie and CSRF token |
| `GET /api/v1/session` | Authenticated session/CSRF state |
| `POST /api/v1/logout` | Revoke session; requires `X-CSRF-Token` |
| `GET /api/v1/capabilities` | Implemented read-only capabilities; execution/capture/terminal support false |
| `GET /api/v1/topologies` | Immutable catalog with hashes and graph counts |
| `GET /api/v1/topologies/{hash}/inventory` | Logical nodes/edges, management attachments, unknown runtime mappings and shared OVS domain |
| `GET /api/v1/diagnostics` | Host OS/kernel/ISA, catalog status and explicit missing runtime checks |

Read the same agent directly from C++ CLI:

```sh
build/dev/lab inspect --socket "$PWD/build/m1-runtime/agent.sock" topologies
build/dev/lab inspect --socket "$PWD/build/m1-runtime/agent.sock" diagnostics
# Supply a hash returned by the catalog:
build/dev/lab inspect --socket "$PWD/build/m1-runtime/agent.sock" inventory sha256:HASH
```

RPC uses a four-byte big-endian length followed by a JSON object with exactly `apiVersion: graphlab.rpc/v1`, `method`, and `params`. Only capabilities, topologies, inventory and diagnostics methods exist. Parameters cannot select host paths or commands. Both ends verify the peer UID (`SO_PEERCRED` on Linux, `getpeereid` on macOS). The agent requires an owned 0700 parent directory and creates its socket under a restrictive umask. M1 uses one unprivileged UID, not the later privileged-agent/separate-API-account deployment.

HTTP requests are bounded to 8 KiB headers and 4 KiB bodies, with five-second asynchronous read/write deadlines and at most 64 active connections. RPC requests are bounded to 4 KiB and responses to 16 MiB, with three-second deadlines and at most 32 agent connections. Each connection handles one request. The catalog is limited to 64 YAML revisions and 4 MiB total canonical topology content. Static assets are allowlisted into memory at startup (16 MiB total); URL paths never open files. Requests with traversal/encoded paths, duplicate security headers, hostile Host/Origin, upgrades or execution methods are rejected. An agent outage returns 503 without a direct backend fallback.

## What the console shows

The graph derives nodes and edges from the catalog, including isolated nodes, disconnected components and parallel edges. The management layer is separately selectable. Inspectors retain port/VLAN/MTU configuration and ordered endpoints. All nodes and data edges also have keyboard-accessible inventory buttons. Dragging changes only temporary view positions.

M1 has **no executor-owned resource mappings**. Runtime identity, observation time, mapping epoch and directional rates are null; runtime, carrier, admin, RSTP and capture states are unknown. Snapshot generation time is separate from runtime observation time. Unknown is not displayed as stopped, healthy, or zero traffic. The host/shared-OVS failure domain is visible even without daemon observations. Diagnostics do not contact Docker, OVS, QEMU or registries. The bundled artifact hashes remain synthetic examples.

## Validation

C++ acceptance tests launch real agent and HTTP child processes, exercise socket credentials and authentication/Origin/CSRF boundaries, and verify all eight topology inventories. Browser tests use the built C++ services rather than a mocked API:

```sh
cd console/web
npx playwright install chromium
npm test
```

The browser test runner reserves loopback port 18088, creates temporary private credentials, stops its services, and removes temporary data. It tests all fixture shapes, parallel paths, management toggling, inspector details, session reload/logout and visible agent failure. Graph-layout tests additionally cover 1, 2, 7, 31 and 100 nodes.

See [verification evidence](validation/m1-verification.md) and the [console screenshot](validation/m1-console.png). GCC 14 and Linux x86-64 qualification remain outstanding from M0. Runtime execution, live resource discovery/ownership, telemetry, terminals, captures, database jobs and separate service accounts belong to later milestones.

Implementation references: [Boost.Beast parser limits](https://www.boost.org/latest/libs/beast/doc/html/beast/ref/boost__beast__http__basic_parser/body_limit.html), [React Flow custom edges](https://reactflow.dev/learn/customization/custom-edges).
