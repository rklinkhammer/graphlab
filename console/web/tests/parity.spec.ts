import { test, expect } from "@playwright/test";
import { spawn, type ChildProcess } from "node:child_process";
import { resolve } from "node:path";
let server: ChildProcess;
test.beforeAll(async () => {
  server = spawn(
    process.execPath,
    [
      "node_modules/vite/bin/vite.js",
      "--host",
      "127.0.0.1",
      "--port",
      "18090",
      "--strictPort",
    ],
    { stdio: "ignore" },
  );
  for (let i = 0; i < 100; i++) {
    try {
      await fetch("http://127.0.0.1:18090/");
      return;
    } catch {
      await new Promise((r) => setTimeout(r, 30));
    }
  }
  throw new Error("fixture unavailable");
});
test.afterAll(async () => {
  if (server?.exitCode === null)
    await new Promise<void>((done) => {
      server.once("exit", () => done());
      server.kill();
    });
});
const runtime = {
  state: "ready",
  observedAt: null,
  mappingEpoch: "m",
  identity: null,
  reason: "",
};
const inventory = {
  topologyHash: "h1",
  topologyId: "lab",
  generatedAt: "2026-09-23",
  runtimeFreshness: "snapshot",
  capturePolicy: { required: false },
  nodes: [
    {
      id: "guest",
      kind: "qemu",
      runtime,
      configuration: {},
      failureDomain: null,
    },
    {
      id: "a",
      kind: "docker",
      runtime,
      configuration: {},
      failureDomain: null,
    },
  ],
  edges: [
    {
      id: "link",
      endpoints: ["guest:p1", "a:p1"],
      runtime,
      adminState: "up",
      carrierState: "up",
      rstpState: "unknown",
      captureState: "active",
      rates: {},
    },
  ],
  management: { networks: {} },
  managementAttachments: [],
  failureDomains: [],
};
const run = {
  id: "r1",
  topologyHash: "h1",
  state: "ready",
  revision: "1",
  resources: [
    { kind: "qemu", logical: "guest" },
    { kind: "container", logical: "a" },
  ],
};
const sample = {
  edge: "link",
  endpoints: ["guest:p1", "a:p1"],
  valid: true,
  source: "rtnetlink/iproute2-stats64",
  mappingEpoch: "epoch",
  counterEpoch: "1",
  forwardMetric: "rx",
  forwardBitsPerSecond: 8000,
  reverseBitsPerSecond: 0,
  forwardPacketsPerSecond: 10,
  reversePacketsPerSecond: 0,
  raw: {
    rxBytes: "9007199254740993",
    txBytes: "100",
    rxPackets: "10",
    txPackets: "0",
    rxErrors: "0",
    txErrors: "0",
    rxDropped: "0",
    txDropped: "0",
  },
  adminUp: true,
  carrierUp: true,
};
async function setup(page: any) {
  const calls: any[] = [];
  let failTelemetry = false;
  await page.route("**/api/v1/**", async (route: any) => {
    const path = new URL(route.request().url()).pathname.slice(8),
      body = route.request().postDataJSON();
    calls.push({ path, body });
    let value: any = {};
    if (path === "session") value = { csrf: "token" };
    else if (path === "capabilities") value = { execution: true };
    else if (path === "topologies")
      value = {
        items: [
          { hash: "h1", id: "lab", nodes: 2, edges: 1 },
          { hash: "h2", id: "other", nodes: 2, edges: 1 },
        ],
      };
    else if (path.endsWith("/inventory"))
      value = {
        ...inventory,
        topologyHash: path.includes("h2") ? "h2" : "h1",
        topologyId: path.includes("h2") ? "other" : "lab",
      };
    else if (path === "runs")
      value = {
        items: [
          { id: "foreign", topologyHash: "h2", state: "ready", revision: "1" },
          run,
        ],
      };
    else if (path === "runs/r1") value = run;
    else if (path === "runs/foreign")
      value = { ...run, id: "foreign", topologyHash: "h2" };
    else if (path.endsWith("/packet-history/query"))
      value = {items: [], segments: [], nextCursor: null};
    else if (path.endsWith("/application-telemetry/query"))
      value = {
        current: [],
        items: [],
        errors: {},
        observationProfile: "full",
        runState: "ready",
      };
    else if (path.endsWith("/telemetry/query")) {
      if (failTelemetry)
        return route.fulfill({
          status: 503,
          json: { error: { code: "collector_unavailable" } },
        });
      value = {
        items: [
          { ...sample, bucketUnixSeconds: "1790200000" },
          { ...sample, bucketUnixSeconds: "1790200001" },
        ],
        current: { link: sample },
      };
    } else if (path.endsWith("/faults")) value = { items: {}, revision: "1" };
    else if (path.endsWith("/timeline"))
      value = {
        items: Array.from({ length: 205 }, (_, i) => ({
          at: String(i).padStart(4, "0"),
          kind: "observation",
          detail: { index: i },
        })),
      };
    else if (path.endsWith("/artifacts")) value = { items: [] };
    else if (path.endsWith("/terminal")) {
      if (body.operation === "list")
        value = {
          items: [
            { id: "serial", node: "guest", kind: "serial" },
            { id: "shell", node: "a", kind: "docker" },
          ],
        };
      else if (body.operation === "acquire") value = { token: "lease" };
      else if (body.operation === "logs")
        value = {
          base64: Buffer.from('boot ready\n<svg onload="alert(1)">\n').toString(
            "base64",
          ),
          source: "QEMU process journal",
          generation: "g1",
          observedAt: "2026-09-23T12:00:00Z",
          limitBytes: 65536,
          truncated: true,
        };
    }
    await route.fulfill({ json: value });
  });
  await page.routeWebSocket("**/api/v1/terminal", (socket: any) => {
    socket.onMessage((raw: string) => {
      const msg = JSON.parse(raw);
      if (msg.params.sequence === "0") {
        const bytes = Buffer.from("SERIAL_READY\r\n"),
          frame = Buffer.alloc(24 + bytes.length);
        frame.writeUInt32BE(1, 16);
        frame.writeUInt32BE(bytes.length, 20);
        bytes.copy(frame, 24);
        socket.send(frame);
      }
      socket.send(JSON.stringify({ status: 200, result: { next: "1" } }));
    });
  });
  await page.goto("http://127.0.0.1:18090/");
  await expect(
    page.getByRole("heading", { name: "lab", exact: true }),
  ).toBeVisible();
  return {
    calls,
    fail: () => {
      failTelemetry = true;
    },
  };
}
test("packet history freezes older pages, filters selection and exposes capture references", async ({page}) => {
  await setup(page);
  let queries: any[] = [];
  const packet = (id: string) => ({id, artifactId: "cap-0", artifactSha256: "sha256:verified", packetIndex: id, blockOffset: "128", timestampUnixMicros: "1700000000000001", edge: "link", interface: "data0", direction: "unknown", capturedLength: 42, originalLength: 80, truncated: true, headers: {protocol: "udp", decodeStatus: "complete", sourceAddress: "10.0.0.1", destinationAddress: "10.0.0.2", sourcePort: 1234, destinationPort: 7777}});
  await page.route("**/packet-history/query", async route => {
    const p = route.request().postDataJSON(); queries.push(p);
    await route.fulfill({json: {items: [packet(p.cursor ? "11" : "22")], segments: [{artifactId: "cap-0", state: "record-limit", indexedRecords: 2000, omittedRecords: "40"}], nextCursor: p.cursor ? null : "cursor-one", captureCoverage: "closed", retainedRecords: 2000, retainedRecordBytes: 10000, generation: "0"}});
  });
  const panel = page.getByRole("region", {name: "Packet history"});
  await panel.getByRole("button", {name: "Return to newest packets"}).click();
  await expect(panel).toContainText("Packet #22");
  await expect(panel).toContainText("Direction: unknown");
  await expect(panel).toContainText("42 / 80 bytes · truncated");
  await panel.getByRole("button", {name: "Older packets"}).click();
  await expect(panel).toContainText("Packet #11");
  const calls = queries.length;
  await page.waitForTimeout(5500);
  expect(queries.length).toBe(calls);
  await expect(panel).toContainText("Older page frozen");
  await panel.getByLabel("Packet protocol").selectOption("udp");
  await expect(panel).toContainText("Packet #22");
  expect(queries.at(-1).cursor).toBeUndefined();
  expect(queries.at(-1).protocol).toBe("udp");
  await page.getByText("Accessible inventory · all nodes and data edges").click();
  await page.getByRole("button", {name: /^link: guest/}).click();
  await expect.poll(() => queries.at(-1).edge).toBe("link");
  await panel.getByText("Segment indexing and omissions (1)").click();
  await expect(panel).toContainText("omitted 40");
  await panel.getByText("Exact observation").click();
  await expect(panel).toContainText("sha256:verified");
  await page.route("**/packet-history/query", route => route.fulfill({status: 409, json: {error: {code: "packet_history_expired"}}}));
  await panel.getByRole("button", {name: "Older packets"}).click();
  await expect(panel.getByRole("alert")).toContainText("packet history expired");
});
test("topology-scoped run, selected serial, logs, lease release and retained sessions", async ({
  page,
}) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));
  const { calls } = await setup(page);
  await expect(page.getByText(/Run r1/)).toBeVisible();
  expect(calls.some((c) => c.path === "runs/foreign")).toBe(false);
  await page
    .getByText("Accessible inventory · all nodes and data edges")
    .click();
  await page
    .getByRole("button", { name: "guest · qemu · runtime ready", exact: true })
    .click();
  await expect(page.getByLabel("Workload", { exact: true })).toHaveValue(
    "guest",
  );
  await page
    .getByRole("button", { name: "Serial console", exact: true })
    .click();
  await expect(page.locator(".xterm-rows")).toContainText("SERIAL_READY");
  await page
    .getByRole("button", { name: "Acquire writer", exact: true })
    .click();
  await expect(page.getByText(/Writer lease active/)).toBeVisible();
  await page
    .getByRole("button", { name: "Release keyboard", exact: true })
    .click();
  await expect
    .poll(
      () => calls.filter((c) => c.body?.operation === "release-writer").length,
    )
    .toBe(1);
  expect(
    calls.find((c) => c.body?.operation === "release-writer").body,
  ).toMatchObject({ sessionId: "serial", params: { token: "lease" } });
  await page.getByRole("button", { name: "Logs", exact: true }).click();
  await page.getByLabel("Search loaded output").fill("boot");
  await expect(page.locator(".console-log")).toHaveText("boot ready");
  await page.getByRole("button", { name: "Pause scrolling" }).click();
  await expect(
    page.getByRole("button", { name: "Resume scrolling" }),
  ).toBeVisible();
  const download = page.waitForEvent("download");
  await page.getByRole("button", { name: "Download retained output" }).click();
  expect((await download).suggestedFilename()).toBe("guest-process.log");
  await page.getByRole("button", { name: "Terminals", exact: true }).click();
  await expect(page.locator(".xterm-rows")).toContainText("SERIAL_READY");
  await page
    .getByRole("region", { name: "Telemetry and faults" })
    .getByRole("button", { name: "link", exact: true })
    .click();
  await expect(page.getByLabel("Console", { exact: true })).toHaveValue(
    "serial",
  );
  await expect(page.locator(".xterm-rows")).toContainText("SERIAL_READY");
  expect(calls.some((c) => c.body?.operation === "open")).toBe(false);
  expect(errors).toEqual([]);
  await page.screenshot({
    path: resolve("..", "..", "docs/validation/web-console-parity.png"),
    fullPage: true,
  });
  await page
    .getByRole("navigation", { name: "Topologies" })
    .getByRole("button", { name: /^other/ })
    .click();
  await expect(page.getByText(/Run foreign/)).toBeVisible();
  await expect(page.getByText(/Run r1/)).toHaveCount(0);
});
test("directional edge selection, precise counters, history pages, stale overlays and narrow layout", async ({
  page,
}) => {
  const fixture = await setup(page);
  await expect(page.locator(".edge-rate")).toContainText("8 kbit/s");
  await page
    .getByText("Accessible inventory · all nodes and data edges")
    .click();
  await page.getByRole("button", { name: /^link: guest/ }).click();
  await expect(
    page.getByRole("region", { name: "Link performance" }),
  ).toContainText("9007199254740993");
  await expect(page.getByLabel("Fault edge")).toHaveValue("link");
  await page
    .getByRole("button", { name: "Set display counter baseline" })
    .click();
  await expect(
    page.getByRole("region", { name: "Link performance" }),
  ).toContainText("Counters since display baseline");
  await page.getByText(/Correlated timeline/).click();
  const history = page.getByRole("region", { name: "Event history" });
  await expect(history.locator("li")).toHaveCount(100);
  await history.getByRole("button", { name: "Load older records" }).click();
  await expect(history.locator("li")).toHaveCount(200);
  await expect(history).toContainText("Paused while browsing older records");
  await history.getByRole("button", { name: "Return to newest" }).click();
  await expect(history.locator("li")).toHaveCount(100);
  fixture.fail();
  await expect(page.locator(".edge-rate")).toHaveText("stale", {
    timeout: 8000,
  });
  await expect(
    page.getByRole("region", { name: "Link performance" }),
  ).toContainText("Stale observation");
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(
    page.getByRole("button", { name: "Serial console", exact: true }),
  ).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBe(true);
});
test("late writer acquisition cannot authorize a different session", async ({
  page,
}) => {
  const { calls } = await setup(page);
  let finish: (() => Promise<void>) | undefined;
  await page.route("**/runs/r1/terminal", async (route) => {
    const body = route.request().postDataJSON();
    if (body.operation !== "acquire") return route.fallback();
    await new Promise<void>((resolve) => {
      finish = async () => {
        await route.fulfill({ json: { token: "late-serial-token" } });
        resolve();
      };
    });
  });
  await page.getByLabel("Console", { exact: true }).selectOption("serial");
  await page
    .getByRole("button", { name: "Acquire writer", exact: true })
    .click();
  await expect.poll(() => Boolean(finish)).toBe(true);
  await page.getByLabel("Console", { exact: true }).selectOption("shell");
  await finish!();
  await expect
    .poll(() =>
      calls.some(
        (c) =>
          c.body?.operation === "release-writer" &&
          c.body.params.token === "late-serial-token",
      ),
    )
    .toBe(true);
  expect(
    calls.find((c) => c.body?.params?.token === "late-serial-token").body
      .sessionId,
  ).toBe("serial");
  await expect(page.getByText(/Read-only viewer/)).toBeVisible();
  await expect(
    page.getByRole("button", { name: "Release keyboard", exact: true }),
  ).toBeDisabled();
});
test("counter epoch reset clears the display baseline and missing rates remain unavailable", async ({
  page,
}) => {
  await setup(page);
  await page
    .getByRole("region", { name: "Telemetry and faults" })
    .getByRole("button", { name: "link", exact: true })
    .click();
  await page
    .getByRole("button", { name: "Set display counter baseline" })
    .click();
  await page.route("**/telemetry/query", (route) =>
    route.fulfill({
      json: {
        items: [],
        current: {
          link: {
            ...sample,
            counterEpoch: "2",
            gapReason: "counter_reset",
            forwardBitsPerSecond: null,
            reverseBitsPerSecond: null,
          },
        },
      },
    }),
  );
  const metrics = page.getByRole("region", { name: "Link performance" });
  await expect(metrics).toContainText("counter_reset");
  await expect(metrics).toContainText("Cumulative interface counters.");
  await expect(page.locator(".edge-rate")).toContainText("A→B —");
  await expect(metrics).toContainText("9007199254740993");
});
test("late fault preview cannot enable a different impairment", async ({
  page,
}) => {
  await setup(page);
  let finish: (() => Promise<void>) | undefined;
  await page.route("**/faults/preview", async (route) => {
    await new Promise<void>((resolve) => {
      finish = async () => {
        await route.fulfill({ json: { edge: "link", delayMs: 100 } });
        resolve();
      };
    });
  });
  const panel = page.getByRole("region", { name: "Telemetry and faults" });
  await panel.getByRole("button", { name: "link", exact: true }).click();
  await panel.getByRole("button", { name: "Preview fault placement" }).click();
  await expect.poll(() => Boolean(finish)).toBe(true);
  await panel.getByLabel("Delay ms").fill("200");
  await finish!();
  await expect(
    panel.getByRole("button", { name: "Apply directional fault" }),
  ).toHaveCount(0);
});
test("application measurements remain separate, unavailable and stale states are explicit", async ({
  page,
}) => {
  await setup(page);
  const sample = {
    node: "a",
    observedAt: "2026-09-23T16:00:00Z",
    gapReason: null,
    report: {
      stream: "udp-echo",
      epoch: "a".repeat(32),
      sequence: "2",
      counters: {
        sentMessages: "9007199254740993",
        receivedMessages: "10",
        sentPayloadBytes: "640",
        receivedPayloadBytes: "640",
        errors: "1",
        rejectedMessages: "2",
        backpressureEvents: "1",
      },
    },
    rates: {
      sentMessagesPerSecond: 2,
      receivedMessagesPerSecond: 2,
      sentPayloadBytesPerSecond: 128,
      receivedPayloadBytesPerSecond: 128,
    },
    latency: {
      kind: "local-service-time",
      count: "10",
      meanUs: 12.5,
      p95UpperBoundUs: 50,
      p95Overflow: false,
    },
  };
  await page.route("**/application-telemetry/query", (route) =>
    route.fulfill({
      json: {
        current: [sample],
        items: [sample],
        errors: {},
        observationProfile: "full",
      },
    }),
  );
  const panel = page.getByRole("region", { name: "Application telemetry" });
  await expect(panel).toContainText("9007199254740993", { timeout: 8000 });
  await expect(panel).toContainText("2 msg/s");
  await expect(panel).toContainText("12.500 µs");
  await expect(panel).toContainText("Not RTT");
  await expect(page.locator(".edge-rate")).toContainText("8 kbit/s");
  await page.route("**/application-telemetry/query", (route) =>
    route.fulfill({
      json: {
        current: [
          {
            ...sample,
            stale: true,
            latency: {
              ...sample.latency,
              p95Overflow: true,
              p95UpperBoundUs: null,
            },
          },
        ],
        items: [],
        errors: { a: "application_report_rejected" },
        observationProfile: "full",
      },
    }),
  );
  await expect(panel).toContainText("Stale / retained observation", {
    timeout: 8000,
  });
  await expect(panel).not.toContainText("2 msg/s");
  await expect(panel.getByRole("alert")).toContainText(
    "application_report_rejected",
  );
  await page.route("**/application-telemetry/query", (route) =>
    route.fulfill({
      json: { current: [], items: [], errors: {}, observationProfile: "full" },
    }),
  );
  await expect(panel).toContainText(
    "Uninstrumented workloads remain supported",
    { timeout: 8000 },
  );
});
