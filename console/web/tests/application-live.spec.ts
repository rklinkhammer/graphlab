import { test, expect } from "@playwright/test";
import { execFileSync, spawn, type ChildProcess } from "node:child_process";
import { resolve } from "node:path";

test("application reports through Linux workload, persistence, authenticated API and browser", async ({ page, request }) => {
  test.skip(!process.env.GRAPHLAB_APPLICATION_LIVE, "Requires isolated Linux application fixture services");
  test.setTimeout(150000);
  const config = process.env.GRAPHLAB_SSH_CONFIG ?? `${process.env.HOME}/.lima/graphlab/ssh.config`;
  const root = "/var/tmp/gl6-application-20260923";
  const source = "/tmp/graphlab-application-20260923";
  const ssh = (command: string) => execFileSync("ssh", ["-F", config, "-o", "BatchMode=yes", "lima-graphlab", command], { encoding: "utf8" });
  const password = ssh(`cat ${root}/password`).trim();
  const base = "http://127.0.0.1:18092";
  let tunnel: ChildProcess | undefined, runId = "";
  try {
    tunnel = spawn("ssh", ["-F", config, "-N", "-o", "ExitOnForwardFailure=yes", "-L", "127.0.0.1:18092:127.0.0.1:18092", "lima-graphlab"], { stdio: "ignore" });
    await expect.poll(async () => { try { return (await fetch(base)).status; } catch { return 0; } }).toBe(200);
    await page.goto(base);
    await page.getByLabel("Operator credential").fill(password);
    await page.getByRole("button", { name: "Sign in", exact: true }).click();
    await page.getByLabel("I accept running without capture coverage.").check();
    await page.getByRole("button", { name: "Start selected topology" }).click();
    await expect(page.getByRole("status").filter({ hasText: "start: succeeded" })).toBeVisible({ timeout: 60000 });
    const runs = await (await page.request.get(`${base}/api/v1/runs`)).json();
    runId = runs.items.find((r: any) => r.state !== "destroyed").id;
    const endpoint = `${base}/api/v1/runs/${runId}/application-telemetry`;
    const read = async () => (await page.request.get(endpoint)).json();
    expect((await request.get(endpoint)).status()).toBe(401);
    expect((await page.request.post(`${endpoint}/query`, { data: {}, headers: { Origin: base } })).status()).toBe(403);
    const { csrf } = await (await page.request.get(`${base}/api/v1/session`)).json();
    const headers = { Origin: base, "X-CSRF-Token": csrf };
    expect((await page.request.post(`${endpoint}/query`, { data: {}, headers: { ...headers, Origin: "http://hostile.invalid" } })).status()).toBe(403);
    expect((await page.request.post(`${endpoint}/query`, { data: { limit: 201 }, headers })).status()).toBe(422);
    await expect.poll(async () => (await read()).current.length, { timeout: 20000 }).toBe(1);
    let data = await read();
    expect(data.current[0].node).toBe("a");
    expect(data.current[0].report.counters.receivedMessages).toBe("0");
    expect(data.current[0].latency.meanUs).toBeNull();
    const detail = await (await page.request.get(`${base}/api/v1/runs/${runId}`)).json();
    const b = detail.health.nodes.find((n: any) => n.id === "b");
    expect(b.gate.applicationTelemetry).toBeUndefined();
    expect(b.containerId).toMatch(/^[a-f0-9]{64}$/);
    for (let i = 0; i < 10; i++) {
      const result = JSON.parse(ssh(`sudo docker exec ${b.containerId} /usr/local/bin/lab-node control probe 10.231.17.1`));
      expect(result.probe).toBe("received");
    }
    await expect.poll(async () => (await read()).current[0].report.counters.receivedMessages, { timeout: 20000 }).toBe("10");
    data = await read();
    const sample = data.current[0];
    expect(sample.report.counters.sentMessages).toBe("10");
    expect(BigInt(sample.report.counters.sentPayloadBytes)).toBeGreaterThan(0n);
    expect(sample.latency.count).toBe("10");
    expect(sample.latency.meanUs).toBeGreaterThanOrEqual(0);
    expect(sample.latency.kind).toBe("local-service-time");
    expect(sample.rates.receivedMessagesPerSecond).toBeGreaterThan(0);
    expect(data.errors).toEqual({});
    const filtered = await (await page.request.post(`${endpoint}/query`, { data: { node: "b", limit: 1 }, headers })).json();
    expect(filtered.current).toEqual([]);
    expect(filtered.items).toEqual([]);
    const limited = await (await page.request.post(`${endpoint}/query`, { data: { node: "a", limit: 1 }, headers })).json();
    expect(limited.items).toHaveLength(1);
    expect(limited.truncated).toBe(true);
    await page.getByText("Accessible inventory · all nodes and data edges").click();
    await page.getByRole("button", { name: /^b · docker · runtime/ }).click();
    const panel = page.getByRole("region", { name: "Application telemetry" });
    await expect(panel).toContainText("No application telemetry reported by b");
    await page.getByRole("button", { name: /^a · docker · runtime/ }).click();
    await expect(panel).toContainText("10 cumulative samples");
    await expect(panel).toContainText("Not RTT or one-way network latency");
    await expect(panel.getByRole("alert")).toHaveCount(0);
    await panel.screenshot({ path: resolve("..", "..", "docs/validation/application-telemetry-live.png") });
    const sequence = sample.report.sequence;
    ssh("sudo systemctl stop graphlab-application-agent");
    ssh(`sudo systemd-run --unit=graphlab-application-agent --property=KillMode=process ${source}/build/dev/lab-agent --socket ${root}/rpc/agent.sock --topologies ${root} --lock ${root}/artifacts.lock.json --state ${root}/state --allow-uid 501`);
    await expect.poll(async () => { try { return (await read()).items.some((s: any) => s.gapReason === "collector_restarted"); } catch { return false; } }, { timeout: 20000 }).toBe(true);
    data = await read();
    expect(data.items.some((s: any) => s.report.sequence === sequence)).toBe(true);
    const resumed = data.items.find((s: any) => s.gapReason === "collector_restarted");
    expect(resumed.rates).toBeNull();
    expect(resumed.report.epoch).toBe(sample.report.epoch);
    console.log(JSON.stringify({ runId, counters: sample.report.counters, latency: sample.latency, rates: sample.rates, retainedReports: data.items.length, restartGap: resumed.gapReason, legacyNodeReports: filtered.current.length }));
  } finally {
    try {
      if (await page.getByRole("button", { name: "Destroy run", exact: true }).isVisible()) {
        await page.getByRole("button", { name: "Destroy run", exact: true }).click();
        await expect(page.getByRole("status").filter({ hasText: "destroy: succeeded" })).toBeVisible({ timeout: 30000 });
        if (runId) {
          const retained = await (await page.request.get(`${base}/api/v1/runs/${runId}/application-telemetry`)).json();
          expect(retained.current.every((s: any) => s.stale)).toBe(true);
        }
      }
    } finally { tunnel?.kill(); }
  }
});
