# Run the console for a specific topology

The console's runtime features require a **Linux execution-enabled agent**, deployable workload images and a started run. Starting `lab-agent` with only `--socket`, `--topologies` and `--lock` intentionally selects the M0/M1 read-only profile. Building the latest frontend does not change that profile.

This walkthrough runs `console-triangle`: two Docker workloads attached to a three-switch OVS triangle, with capture enabled on all five data edges. Node `b` generates GLM1 requests to node `a`; both report message observations. It requires no QEMU disk or guest login setup. To run an existing deployable topology instead, use the substitution instructions below step 4.

## 1. Use a Linux execution host

Run all build and service commands below **on the Linux host**, from the Graphlab repository root, in an ordinary user's Bash shell. Use `sudo` only where shown. On macOS, use a Linux VM or remote Linux host; macOS can build the read-only console but cannot execute the topology. Docker Desktop alone is not a substitute for the Linux host's systemd, OVS and network namespace services.

Required on that host:

- CMake 3.28+, Ninja, a C++23 compiler/library supporting `std::expected`, Boost 1.92.0 CMake configuration, OpenSSL 3, SQLite 3.24+ and libpcap development files.
- Node/npm for the frontend, and `jq` for the setup commands below. Playwright/Chromium is needed only for browser tests, not for running the console.
- A rootful Docker Engine with API **1.52 or newer**, systemd, Open vSwitch and iproute2. Graphlab uses `/var/run/docker.sock` and `/usr/bin/ovs-vsctl`.
- Available subnets: this example's data interfaces use `10.231.17.0/24`. Choose a host where this does not conflict with existing routes. This walkthrough creates no management network.
- Space for builds/images and captures. The browser's default capture policy budgets 6 GiB and reserves 5 GiB free; provision sufficient space on the state filesystem. Step 7 gives a bounded small-lab alternative.

Check the runtime services before proceeding:

```sh
sudo docker version --format '{{.Server.APIVersion}}'
sudo ovs-vsctl show
systemctl is-system-running
ip route
```

A degraded systemd host may have unrelated failed units; inspect those rather than assuming systemd cannot launch workers. Use a dedicated lab host. Only one Graphlab executor can own the host at a time; destroy/recover existing runs through their owning agent before switching configurations.

## 2. Build and install the current controller and console

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
npm ci --prefix console/web
npm run build --prefix console/web

sudo cmake --install build/dev --prefix /opt/graphlab-console
sudo install -d -m 755 /opt/graphlab-console/web
sudo cp -R console/web/dist/. /opt/graphlab-console/web/
sudo chown -R root:root /opt/graphlab-console
sudo chmod -R go-w /opt/graphlab-console
```

Use the `linux-gcc14` preset throughout instead of `dev` if that is your configured toolchain, and substitute `build/linux-gcc14` in subsequent commands. Install `lab-agent`, `lab-capture` and `lab-terminal` together: the agent locates workers beside its own executable, and workers must be root-owned regular files without group/other write permissions. Do not replace installed binaries while a run is active.

## 3. Build two real workload images

Install the SDK from this build, then build its independent consumers on the same Linux architecture. SDK 1.6.0 is the version used by the current message fixtures.

```sh
cmake --install build/dev/packages/lab-support --prefix "$PWD/build/console-sdk"
tar -C build/console-sdk -cf build/console-sdk-1.6.0.tar .

for node in app-message-target app-messages; do
  cmake -S "docker-nodes/$node" -B "build/console-$node" -G Ninja \
    -DCMAKE_PREFIX_PATH="$PWD/build/console-sdk" -DLAB_SUPPORT_VERSION=1.6.0
  cmake --build "build/console-$node"
done
```

Use a locally installed base image compatible with your compiler host's glibc and OpenSSL runtime. The example below uses Ubuntu 24.04; if you build on another distribution, choose a matching base. Prepare the base with the OpenSSL runtime, resolve its immutable image ID, and build the workload images without network access. The lock in step 4 pins the resulting image IDs, not mutable tags.

```sh
mkdir -p build/console-base
cat > build/console-base/Dockerfile <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libssl3t64 \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE
sudo docker build --pull -t graphlab-console/base:local build/console-base
console_base_id=$(sudo docker image inspect graphlab-console/base:local --format '{{.Id}}')
for node in app-message-target app-messages; do
  mkdir -p "build/console-image-$node"
  cp "build/console-$node/lab-node" "build/console-image-$node/lab-node"
  cp docker-nodes/app-a/Dockerfile "build/console-image-$node/Dockerfile"
  sudo docker build --network=none --build-arg "BASE_IMAGE=$console_base_id" \
    -t "graphlab-console/$node:local" "build/console-image-$node"
done
```

Do not run these workload containers manually: the agent creates them with the required identity, interfaces and gate configuration. Image build success alone does not prove runtime-library compatibility; an immediate container exit during Start should be checked for missing shared libraries or glibc version errors.

## 4. Prepare exactly one topology and its artifact lock

The repository's `topologies/triangle.yaml` includes a QEMU guest and synthetic `registry.invalid` image references. It is a validation example, not a ready-to-start runtime configuration. The following uses the existing fixture generator to create the Docker-only triangle, then replaces its inherited test package/image metadata with the SDK archive and images just built.

Use a new directory for this setup; these commands are intended for initial preparation, not editing a live catalog.

```sh
mkdir -p build/console-triangle
console_a_id=$(sudo docker image inspect graphlab-console/app-message-target:local --format '{{.Id}}')
console_b_id=$(sudo docker image inspect graphlab-console/app-messages:local --format '{{.Id}}')
console_arch=$(sudo docker image inspect graphlab-console/app-messages:local --format '{{.Architecture}}')
console_sdk_sha="sha256:$(sha256sum build/console-sdk-1.6.0.tar | cut -d ' ' -f 1)"

build/dev/m2_tests --fixtures "$PWD" "$PWD/build/console-triangle" "$console_a_id"
jq --arg a "graphlab.local/app-a@$console_a_id" \
   --arg b "graphlab.local/app-b@$console_b_id" \
   --arg platform "linux/$console_arch" --arg sdk "$console_sdk_sha" '
  .workloads["app-a"].image = $a |
  .workloads["app-b"].image = $b |
  .workloads |= with_entries(
    .value.platform = $platform |
    .value.contract.platforms = [$platform] |
    .value.contract.labSupport.version = "1.6.0" |
    .value.contract.labSupport.packageSha256 = $sdk
  )' build/console-triangle/artifacts.lock.json > build/console-triangle/lock.next.json
mv build/console-triangle/lock.next.json build/console-triangle/artifacts.lock.json

for workload in app-a app-b; do
  jq --arg name "$workload" '.workloads[$name].contract' \
    build/console-triangle/artifacts.lock.json > build/console-triangle/contract.json
  console_contract_sha=$(build/dev/lab hash build/console-triangle/contract.json)
  jq --arg name "$workload" --arg sha "$console_contract_sha" \
    '.workloads[$name].contractSha256 = $sha' \
    build/console-triangle/artifacts.lock.json > build/console-triangle/lock.next.json
  mv build/console-triangle/lock.next.json build/console-triangle/artifacts.lock.json
done
console_lock_sha=$(build/dev/lab hash build/console-triangle/artifacts.lock.json)
jq --arg sha "$console_lock_sha" \
  '.id = "console-triangle" | .capture.required = true | .artifactLock = $sha' \
  build/console-triangle/m2.yaml > build/console-triangle/console-triangle.yaml
rm build/console-triangle/m2.yaml build/console-triangle/contract.json

build/dev/lab validate build/console-triangle/console-triangle.yaml \
  --lock build/console-triangle/artifacts.lock.json
```

This example retains the fixture workload contracts; the artifact archive digest records your local installed SDK bytes, not a published release or publisher attestation.

The generator writes JSON in a `.yaml` file, which is accepted by Graphlab's parser. The catalog scans only `.yaml`/`.yml` files. Validation must succeed before launch; it checks metadata consistency, while runtime admission also verifies installed image bytes, architecture and host resources. Keep the SDK archive for its recorded package identity.

Install the two configuration files in a root-owned directory:

```sh
sudo install -d -m 755 /etc/graphlab/console-triangle
sudo install -m 644 build/console-triangle/console-triangle.yaml \
  build/console-triangle/artifacts.lock.json /etc/graphlab/console-triangle/
```

**Using your own topology:** replace these two installed files with your validated topology and its matching lock. Keep only the intended `.yaml`/`.yml` revisions in the catalog directory; `--topologies` accepts a directory, not a filename. Every referenced image must already be installed on this Linux Docker daemon with the lock's immutable identity and native architecture. Every changed contract requires a new `contractSha256`; every changed lock requires updating the topology's `artifactLock` using `lab hash`. A QEMU topology also requires its real disk/template/keys and [QEMU setup](m4-qemu-consoles.md). The catalog is loaded at agent startup; edits require an orderly restart after run cleanup.

## 5. Create runtime directories and the operator credential

Run as the ordinary user who will run the API:

```sh
console_gid=$(id -g)
sudo install -d -m 700 /var/lib/graphlab-console-triangle
sudo install -d -m 750 -o root -g "$console_gid" /run/graphlab-console-triangle
mkdir -p "$HOME/.config/graphlab-console"
chmod 700 "$HOME/.config/graphlab-console"
/opt/graphlab-console/bin/lab-api init-auth "$HOME/.config/graphlab-console/auth.json"
```

Save the credential printed once by `init-auth`; enter it in the browser. Do not run `init-auth` with sudo. If the file already exists, reuse its credential; initialization refuses to overwrite it. `/run` is ephemeral, so recreate the socket directory after a host reboot. Preserve the state directory across service restarts.

## 6. Launch the execution-enabled agent and API

In **Linux terminal A**, as the same ordinary user, run:

```sh
sudo /opt/graphlab-console/bin/lab-agent \
  --socket /run/graphlab-console-triangle/agent.sock \
  --topologies /etc/graphlab/console-triangle \
  --lock /etc/graphlab/console-triangle/artifacts.lock.json \
  --state /var/lib/graphlab-console-triangle \
  --allow-uid "$(id -u)"
```

Leave it running. Both final options are required for execution. `--allow-uid` is the unprivileged API user's UID, evaluated before sudo; it is not root's UID. The agent sets the socket group to that user's primary group.

In **Linux terminal B**, as that user **without sudo**, run:

```sh
/opt/graphlab-console/bin/lab-api \
  --socket /run/graphlab-console-triangle/agent.sock \
  --auth "$HOME/.config/graphlab-console/auth.json" \
  --assets /opt/graphlab-console/web \
  --port 8088 \
  --agent-uid 0
```

Leave it running. `--agent-uid 0` tells the API to authenticate the root agent. The API itself refuses to run as root. Keep the shown flag order: the current CLIs parse ordered arguments.

In **Linux terminal C**, verify the live agent before opening the browser:

```sh
printf '{}\n' > /tmp/graphlab-console-query.json
/opt/graphlab-console/bin/lab control \
  --socket /run/graphlab-console-triangle/agent.sock --agent-uid 0 \
  capabilities /tmp/graphlab-console-query.json
/opt/graphlab-console/bin/lab control \
  --socket /run/graphlab-console-triangle/agent.sock --agent-uid 0 \
  topologies /tmp/graphlab-console-query.json
```

Expect `readOnly: false`, `execution: true`, `captures: true` and `recordedTerminals: true`, and catalog ID `console-triangle`. Capability flags indicate implemented support; they do not replace the start job's host/image checks. `lab preflight` is the static M0 report and does **not** inspect this running agent.

## 7. Open the console and start the selected topology

On the Linux desktop, open **http://127.0.0.1:8088**. From another machine, keep an SSH tunnel open:

```sh
ssh -N -L 127.0.0.1:8088:127.0.0.1:8088 YOUR_LINUX_SSH_HOST
```

For the existing Lima VM from macOS, the equivalent is:

```sh
ssh -F "$HOME/.lima/graphlab/ssh.config" -N \
  -L 127.0.0.1:8088:127.0.0.1:8088 lima-graphlab
```

Use `127.0.0.1`, not `localhost`, and preserve port 8088 at both ends; Host/Origin validation uses that authority. Stop the old read-only API/tunnel if it already occupies this port, or consistently choose another API/local-forward port.

1. Sign in with the credential from step 5.
2. Select **console-triangle** in the topology catalog. Loading the graph does not start it.
3. Click **Start selected topology**. The panel should say **M3 capture-first execution**, not request acceptance of missing capture coverage.
4. Wait for the start job to succeed and the run to become **ready**. The agent creates resources, arms every required capture and only then releases workload traffic.

For a small lab filesystem, instead of clicking Start, submit a smaller policy through the same agent from terminal C, then refresh the console:

```sh
/opt/graphlab-console/bin/lab control \
  --socket /run/graphlab-console-triangle/agent.sock --agent-uid 0 \
  topologies /tmp/graphlab-console-query.json > /tmp/graphlab-console-catalog.json
console_topology_hash=$(jq -r '.items[] | select(.id == "console-triangle") | .hash' \
  /tmp/graphlab-console-catalog.json)
jq -n --arg hash "$console_topology_hash" --arg key "$(cat /proc/sys/kernel/random/uuid)" \
  '{topologyHash:$hash,idempotencyKey:$key,capturePolicy:{runBytes:67108864,reserveBytes:1048576,rotateBytes:1048576,rotateSeconds:10}}' \
  > /tmp/graphlab-console-start.json
/opt/graphlab-console/bin/lab control \
  --socket /run/graphlab-console-triangle/agent.sock --agent-uid 0 \
  start /tmp/graphlab-console-start.json
```

This example limits capture allocation to 64 MiB and reserves 1 MiB free. It is a short lab run, not a long-running capture policy. Keep the same request file/key when retrying an uncertain request. Do not submit both start methods for the same experiment.

## 8. Inspect traffic and finish the run

- Select node **b** using the graph or accessible inventory. After a few collection cycles, **Application message observations** shows send/receive events from the generator; **Source controls** can pause alpha or beta independently.
- Select a network edge to view its interface counters and available rates. These are distinct from application messages. This fixture does not emit every optional aggregate application metric, so unavailable fields are expected.
- Use **Consoles** for the Docker process console and logs. This Docker-only example has no QEMU serial port. A guest serial login requires a QEMU topology with a guest-provided login service.
- Use **Captures** for finalized PCAPNG artifacts and verified downloads. Packet history indexes finalized segments, so wait for rotation or click **Quiesce** to close the current segments. **Quiesce** preserves resources and stops generation; **Destroy** removes runtime resources.
- After quiescing, select node **b**, choose correlation edge **b-s**, and correlate a retained message event. The union of several capture edges can legitimately be ambiguous. No matching retained observation is not evidence of delivery loss.
- Finish with **Destroy**, and wait for the job to succeed. If a failed/interrupted run is `reconciling`, use **Recover and clean up**. Preserve retained captures and the state database. Stop API and agent with Ctrl-C only after runtime cleanup; stopping the services is not a substitute for destroying a run.

## Troubleshooting

| Symptom | Check / action |
|---|---|
| Only M0/M1 or read-only capabilities | Use step 6's root agent with **both** `--state` and `--allow-uid`; verify live `capabilities` via `lab control`. Confirm the browser/tunnel reaches the new API rather than an older process. |
| No Start button | Confirm `execution: true`, the selected topology and current run state. Existing runs have lifecycle controls instead. |
| `lab-api must run as an unprivileged user` | Run the API without sudo; retain `--agent-uid 0`. |
| Socket permission/peer rejection | Check allowed UID, primary group, `/run` directory traversal permissions and API agent UID. Do not make the socket world-writable. |
| Existing socket or host executor already running | Identify the owning agent. Clean up its runs and stop it. Remove only its stale socket after confirming it has exited; keep its state for recovery. |
| Unknown topology/hash or lock mismatch | Validate the exact installed topology/lock, recompute hashes in contract → lock → topology order, then restart the agent to reload its catalog. |
| Image missing/digest/platform mismatch | Build/load images into this host's rootful Docker daemon and update the lock with inspected immutable IDs and native platform. Graphlab does not pull images at start. |
| Capture/terminal worker rejected | Install root-owned workers beside the installed agent; confirm systemd and file permissions. |
| Start job failed, disk capacity error | Inspect the job error, state filesystem free space and capture policy. Use the small-lab policy only if appropriate; recover failed resources before a new run. |
| No packet history yet | Wait for finalized segments or Quiesce; active `.partial` captures are not indexed. |
| No message/telemetry/serial data | Features depend on the workload and run. Use the opt-in fixtures for messages; ordinary workloads need not report telemetry. Serial requires QEMU and guest support. |
| UI appears older than the build | Rebuild and recopy `dist`, then restart the API and sign in again; it reads assets at startup. |

For viewing topology definitions without execution, use the separately labeled [read-only profile](m1-console.md). Feature contracts and current limits are in the [parity report](web-console-feature-parity.md).

Documentation check: [shell syntax and Linux topology/lock validation](validation/console-startup-docs.txt). The metadata check reused qualified images/SDK bytes; it did not rebuild an Ubuntu base or launch another experiment. See the message increment evidence for the runtime fixture qualification.
