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
async function setup(page: any, application: any = null) {
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
        application,
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
    else if (path.endsWith("/messages/query")) value={items:[],sources:[],nextCursor:null};
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
test("application and network views use declared mappings without inferred metrics", async ({page}) => {
  await setup(page, {apiVersion: "graphlab.application-dataflow/v1", edges: [
    {id: "echo", source: "guest", target: "a", networkEdges: ["link"]},
    {id: "unmapped", source: "a", target: "guest", networkEdges: []},
  ]});
  const panel = page.getByRole("region", {name: "Application dataflow mapping"});
  await panel.getByRole("button", {name: "echo: guest → a", exact: true}).click();
  await expect(page.getByLabel("Topology view")).toHaveValue("application");
  await expect(page.getByLabel("Application edge echo: guest to a", {exact: true})).toBeVisible();
  await expect(panel.getByText(/not an observed route/)).toBeVisible();
  await page.screenshot({path: resolve("../../docs/validation/application-dataflow.png"), fullPage: true});
  await panel.getByRole("button", {name: "Inspect network edge link"}).click();
  await expect(page.getByLabel("Topology view")).toHaveValue("network");
  await expect(panel.getByText("· maps selected network edge")).toBeVisible();
  await expect(page.getByLabel("Data edge link: guest:p1 to a:p1; runtime ready", {exact: true})).toHaveClass(/selected/);
  await panel.getByRole("button", {name: "unmapped: a → guest", exact: true}).click();
  await expect(panel.getByText("Network mapping unspecified.")).toBeVisible();
  await panel.getByRole("button", {name: "Inspect source a"}).click();
  await expect(page.getByRole("button", {name: "Open node console"})).toBeVisible();
});
test("application edge reports stay endpoint scoped with exact counters and unavailable metrics", async ({page}) => {
  const protocol = {apiVersion:"graphlab.application-protocol/v1",transport:"tcp",framing:"fixed-16",schema:"echo/v1"};
  const fixture = await setup(page, {apiVersion:"graphlab.application-dataflow/v1",edges:[
    {id:"alpha",source:"guest",target:"a",networkEdges:["link"],protocol},
    {id:"beta",source:"guest",target:"a",networkEdges:["link"]},
  ]});
  await page.route("**/application-telemetry/query", route => {
    const edge = route.request().postDataJSON().edge;
    const sample = {node:"a",observedAt:"2026-09-23",stale:edge==="beta",report:{edge,endpoint:"target",stream:edge,epoch:"a".repeat(32),sequence:"2",counters:{sentMessages:null,sentPayloadBytes:null,receivedMessages:edge==="alpha"?"9007199254740993":"7",receivedPayloadBytes:"16",errors:"0",rejectedMessages:"0",backpressureEvents:null,backpressureNs:null,reconnects:"3"}},rates:{receivedMessagesPerSecond:1},latency:null};
    return route.fulfill({json:{current:edge?[sample]:[],items:[],errors:{},observationProfile:"full"}});
  });
  await page.getByRole("button",{name:"alpha: guest → a",exact:true}).click();
  const panel=page.getByRole("region",{name:"Application telemetry"});
  await expect(panel).toContainText("Selected application edge alpha");
  await expect(panel).toContainText("9007199254740993 messages");
  await expect(panel).toContainText("target reporter");
  await expect(panel).toContainText("Reconnects: 3");
  await expect(panel).toContainText("backpressure duration: unavailable");
  await expect(panel.getByText("Sent",{exact:true})).toHaveCount(0);
  await expect(page.getByText(/Declared protocol: tcp/)).toBeVisible();
  await page.getByRole("button",{name:"beta: guest → a",exact:true}).click();
  await expect(panel).toContainText("Selected application edge beta");
  await expect(panel).toContainText("7 messages");
  await expect(panel).not.toContainText("9007199254740993");
  await expect(panel).toContainText("Stale / retained observation");
});
test("legacy topology has no invented application edges", async ({page}) => {
  await setup(page);
  await page.getByLabel("Topology view").selectOption("application");
  await expect(page.getByText("No application edges declared.")).toBeVisible();
  await expect(page.locator('.react-flow__edge')).toHaveCount(0);
});
test("packet maintenance requires explicit global scope and displays rebuild state", async ({page}) => {
  await setup(page);
  let last: any;
  await page.route("**/packet-history/rebuild", async route => {last=route.request().postDataJSON();await route.fulfill({json:{state:"queued"}});});
  await page.route("**/packet-history/recover", async route => {last=route.request().postDataJSON();await route.fulfill({json:{state:"completed"}});});
  await page.route("**/packet-history/query", route=>route.fulfill({json:{items:[],segments:[],nextCursor:null,retirementBoundary:"2026-09-23T00:00:00Z/run/cap/000",retiredCatalogEntries:20,scanWindowTruncated:true,maintenance:{runId:"r1",state:"interrupted"}}}));
  const panel=page.getByRole("region",{name:"Packet history"});
  await panel.getByRole("button",{name:"Return to newest packets"}).click();
  await expect(panel).toContainText("Catalog entries recycled: 20");
  await expect(panel).toContainText("Only the newest 1,000");
  await panel.getByText("Packet index maintenance",{exact:true}).click();
  await expect(panel).toContainText("Last rebuild: interrupted");
  await expect(panel.getByRole("button",{name:"Recover packet index for all runs",exact:true})).toBeDisabled();
  await panel.getByRole("button",{name:"Rebuild this run's packet index",exact:true}).click();
  await expect(panel.getByRole("status")).toContainText("rebuild: queued");
  expect(last).toEqual({});
  await panel.getByLabel("Replace derived packet indexes for all runs; preserve capture files.").check();
  await panel.getByRole("button",{name:"Recover packet index for all runs",exact:true}).click();
  await expect(panel.getByRole("status")).toContainText("recover: completed");
  expect(last).toEqual({scope:"all-runs"});
  await expect(panel.getByLabel("Replace derived packet indexes for all runs; preserve capture files.")).not.toBeChecked();
});
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

test("source controls bind identity, show outcomes and fence late selection responses", async ({page}) => {
  await setup(page);
  let release:()=>void=()=>{};let sent:any;
  await page.route("**/source-controls/query",async route=>{
    const {node}=route.request().postDataJSON();
    await route.fulfill({json:{runState:"stopped",instance:"instance-b",commands:[],capability:node==="a"?{apiVersion:"graphlab.source-control/v1",epoch:"epoch-b",sources:[{id:"alpha",state:"running",generatedDatagrams:"9007199254740993"}]}:null}});
  });
  await page.route("**/source-controls/command",async route=>{
    sent=route.request().postDataJSON();await new Promise<void>(r=>{release=r;});await route.fulfill({json:{request:sent,outcome:"acknowledged"}});
  });
  await page.getByText("Accessible inventory · all nodes and data edges").click();
  await page.getByRole("button",{name:"a · docker · runtime ready",exact:true}).click();
  const panel=page.getByRole("region",{name:"Source controls"});
  await expect(panel).toContainText("9007199254740993");
  await panel.getByRole("button",{name:"Pause source alpha",exact:true}).click();
  await expect(panel).toContainText("requested");await expect(panel.getByRole("button",{name:"Resume source alpha",exact:true})).toBeDisabled();
  expect(sent).toMatchObject({node:"a",instance:"instance-b",epoch:"epoch-b",source:"alpha",action:"pause"});
  await page.getByRole("button",{name:"guest · qemu · runtime ready",exact:true}).click();release();
  await expect(panel).toContainText("Target: guest");await expect(panel.getByRole("button",{name:/Pause source/})).toHaveCount(0);await expect(panel).not.toContainText("acknowledged");
});

for (const status of [409,429]) test(`source command rejection ${status} stays visible and permits a fresh identity-bound action`, async ({page}) => {
  await setup(page);
  let epoch="a".repeat(32), queries=0;
  const requests:any[]=[];
  await page.route("**/source-controls/query",async route=>{
    queries++;
    await route.fulfill({json:{runState:"ready",instance:`instance-${epoch[0]}`,commands:[],capability:{apiVersion:"graphlab.source-control/v1",epoch,sources:[{id:"alpha",state:"running",generatedDatagrams:"4"}]}}});
  });
  await page.route("**/source-controls/command",async route=>{
    const body=route.request().postDataJSON();requests.push(body);
    if(requests.length===1){epoch="b".repeat(32);await route.fulfill({status,json:{error:{code:status===409?"source_epoch_changed":"source_command_capacity"}}});}
    else await route.fulfill({json:{request:body,outcome:"acknowledged"}});
  });
  await page.getByText("Accessible inventory · all nodes and data edges").click();
  await page.getByRole("button",{name:"a · docker · runtime ready",exact:true}).click();
  const panel=page.getByRole("region",{name:"Source controls"});
  await panel.getByRole("button",{name:"Pause source alpha",exact:true}).click();
  await expect(panel.getByRole("status")).toContainText("failed");
  await expect(panel.getByRole("button",{name:"Pause source alpha",exact:true})).toBeEnabled();
  await expect.poll(()=>queries).toBeGreaterThan(2);
  await expect(panel.getByRole("alert")).toContainText(status===409?"source epoch changed":"source command capacity");
  await expect(panel.getByRole("status")).toContainText("failed");
  if(status===409)await panel.screenshot({path:resolve("../../docs/validation/source-control-remediation-recovery.png")});
  await panel.getByRole("button",{name:"Resume source alpha",exact:true}).click();
  await expect(panel.getByRole("status")).toContainText("acknowledged");
  await expect(panel.getByRole("button",{name:"Pause source alpha",exact:true})).toBeEnabled();
  expect(requests[1]).toMatchObject({instance:"instance-b",epoch:"b".repeat(32),action:"resume"});
  expect(requests[1].requestId).not.toBe(requests[0].requestId);
});

for(const failure of ["server","network"])test(`uncertain source ${failure} failure retains retry across successful polling`, async({page})=>{
  await setup(page);
  let queries=0;
  const requests:any[]=[];
  await page.route("**/source-controls/query",async route=>{
    queries++;
    await route.fulfill({json:{runState:"ready",instance:"instance-a",commands:[],capability:{apiVersion:"graphlab.source-control/v1",epoch:"a".repeat(32),sources:[{id:"alpha",state:"running",generatedDatagrams:"4"}]}}});
  });
  await page.route("**/source-controls/command",async route=>{
    const body=route.request().postDataJSON();requests.push(body);
    if(requests.length===1){if(failure==="network")await route.abort("failed");else await route.fulfill({status:503,json:{error:{code:"agent_unavailable"}}});}
    else await route.fulfill({json:{request:body,outcome:"acknowledged"}});
  });
  await page.getByText("Accessible inventory · all nodes and data edges").click();
  await page.getByRole("button",{name:"a · docker · runtime ready",exact:true}).click();
  const panel=page.getByRole("region",{name:"Source controls"});
  await panel.getByRole("button",{name:"Pause source alpha",exact:true}).click();
  await expect(panel.getByRole("status")).toContainText("unknown");
  await expect.poll(()=>queries).toBeGreaterThan(2);
  await expect(panel.getByRole("button",{name:"Pause source alpha",exact:true})).toBeDisabled();
  await expect(panel.getByRole("alert")).toBeVisible();
  await panel.getByRole("button",{name:"Retry same request",exact:true}).click();
  await expect(panel.getByRole("status")).toContainText("acknowledged");
  await expect(panel.getByRole("button",{name:"Pause source alpha",exact:true})).toBeEnabled();
  expect(requests[1]).toEqual(requests[0]);
});

test("retained process logs freeze pages, scope search and verify exact-byte downloads",async({page})=>{
 await setup(page);let queries=0;const bytes=Buffer.from([0,255,60,115,99,114,105,112,116,62,10]);
 const source={id:"qemu-worker",stale:true,evicted:"2",error:""};
 const item=(id:string)=>({id,sourceId:"qemu-worker",observedAt:"2026-09-23",size:"11",gap:"snapshot_boundary",generation:"g"});
 await page.route("**/process-logs/query",async route=>{queries++;const p=route.request().postDataJSON();expect(p.node).toBe("guest");await route.fulfill({json:{sources:[source],items:[item(p.cursor?"1":"2")],nextCursor:p.cursor?null:"frozen-page",coverage:"bounded"}});});
 await page.route("**/process-logs/download",async route=>{const p=route.request().postDataJSON();expect(p.node).toBe("guest");await route.fulfill({json:{...item(p.id),sha256:"fixture-checksum",base64:bytes.toString("base64")}});});
 await page.getByText("Accessible inventory · all nodes and data edges").click();await page.getByRole("button",{name:"guest · qemu · runtime ready",exact:true}).click();await page.getByRole("button",{name:"Logs",exact:true}).click();await page.getByRole("button",{name:"Retained log artifacts",exact:true}).click();
 const panel=page.getByRole("region",{name:"Retained process logs"});await expect(panel).toContainText("2 snapshots evicted");await panel.getByRole("button",{name:"Older log snapshots",exact:true}).click();await expect(panel).toContainText("Frozen older page");await expect(panel.getByRole("button",{name:"Inspect log 1",exact:true})).toBeVisible();const frozen=queries;await page.waitForTimeout(3300);expect(queries).toBe(frozen);
 await panel.getByRole("button",{name:"Inspect log 1",exact:true}).click();await expect(panel.locator("pre")).toContainText("<script>");expect(await panel.locator("script").count()).toBe(0);await expect(panel).toContainText("invalid UTF-8");
 const pending=page.waitForEvent("download");await panel.getByRole("button",{name:"Download log 1",exact:true}).click();const download=await pending;const stream=await download.createReadStream();const chunks:Buffer[]=[];for await(const chunk of stream!)chunks.push(Buffer.from(chunk));expect(Buffer.concat(chunks)).toEqual(bytes);
 await panel.getByLabel("Search selected snapshot").fill("absent");await expect(panel.locator("pre")).toHaveText("");await panel.getByLabel("Search selected snapshot").fill("<script>");await expect(panel.locator("pre")).toContainText("<script>");
 await panel.screenshot({path:resolve("../../docs/validation/process-logs-browser.png")});await panel.getByRole("button",{name:"Return to newest logs",exact:true}).click();await expect(panel.getByRole("button",{name:"Inspect log 2",exact:true})).toBeVisible();
 await panel.getByLabel("Log source").selectOption("qemu-worker");await expect(panel.locator("pre")).toHaveCount(0);await expect(panel.getByRole("button",{name:"Inspect log 2",exact:true})).toBeVisible();
 await page.route("**/process-logs/download",r=>r.fulfill({status:409,json:{error:{code:"log_artifact_checksum_mismatch"}}}));await panel.getByRole("button",{name:"Download log 2",exact:true}).click();await expect(panel.getByRole("alert")).toContainText("checksum mismatch");
});

for(const failure of ["checksum","network"])test(`retained log ${failure} error survives catalog polling until retry or scope change`,async({page})=>{
 await setup(page);let queries=0,failDownload=true,failQuery=false;
 const item={id:"2",sourceId:"qemu-worker",size:"8",gap:"snapshot_boundary",generation:"g"};
 await page.route("**/process-logs/query",async route=>{
   queries++;
   if(failQuery)return route.fulfill({status:503,json:{error:{code:"log_database_error"}}});
   await route.fulfill({json:{sources:[{id:"qemu-worker",evicted:"0",stale:false}],items:[{...item,observedAt:`poll-${queries}`}],nextCursor:null}});
 });
 await page.route("**/process-logs/download",async route=>{
   if(failDownload){if(failure==="network")return route.abort("failed");return route.fulfill({status:409,json:{error:{code:"log_artifact_checksum_mismatch"}}});}
   await route.fulfill({json:{...item,sha256:"fixture",base64:Buffer.from("retained").toString("base64")}});
 });
 await page.getByText("Accessible inventory · all nodes and data edges").click();await page.getByRole("button",{name:"guest · qemu · runtime ready",exact:true}).click();await page.getByRole("button",{name:"Logs",exact:true}).click();await page.getByRole("button",{name:"Retained log artifacts",exact:true}).click();
 const panel=page.getByRole("region",{name:"Retained process logs"}),artifactError=panel.getByRole("alert").filter({hasText:"Artifact:"}),catalogError=panel.getByRole("alert").filter({hasText:"Catalog:"});
 await panel.getByRole("button",{name:"Download log 2",exact:true}).click();await expect(artifactError).toBeVisible();if(failure==="checksum")await expect(artifactError).toContainText("checksum mismatch");
 const before=queries;await expect.poll(()=>queries).toBeGreaterThan(before);await expect(panel).toContainText(`poll-${queries}`);await expect(artifactError).toBeVisible();
 // A failed catalog poll and its recovery must not erase the action failure either.
 failQuery=true;await expect(catalogError).toBeVisible();await expect(artifactError).toBeVisible();
 failDownload=false;await panel.getByRole("button",{name:"Inspect log 2",exact:true}).click();await expect(panel.locator("pre")).toHaveText("retained");await expect(artifactError).toHaveCount(0);await expect(catalogError).toBeVisible();
 failQuery=false;await expect(catalogError).toHaveCount(0);
 failDownload=true;await panel.getByRole("button",{name:"Download log 2",exact:true}).click();await expect(artifactError).toBeVisible();await panel.getByLabel("Log source").selectOption("qemu-worker");await expect(artifactError).toHaveCount(0);await expect(panel.locator("pre")).toHaveCount(0);
});

test("capture metadata shows provenance and unavailable legacy fields; corrupt downloads fail",async({page})=>{
 await setup(page);
 await page.route("**/artifacts",r=>r.fulfill({json:{items:[{id:"cap-0",edge:"link",epoch:"1",state:"closed",size:"3",sha256:"sha256:wrong",format:"pcapng",linkType:1,canonicalEndpoint:"guest:p1",captureInterface:"tap0",mappingEpoch:"mapping-1",captureCoverage:"closed",closedAt:"2026-09-23T12:00:00Z",packets:"2",packetLengths:{capturedBytes:"60",originalBytes:"90",truncatedPackets:"1"},limits:{snaplen:65535,byteBudget:1048576,rotateBytes:65536,rotateSeconds:5},provenance:"owned capture descriptor and finalized worker manifest; bytes verified on download"},{id:"legacy-0",edge:"link",state:"closed",size:"3",sha256:"sha256:wrong"}]}}));
 await page.route("**/artifacts/cap-0/chunks/0",r=>r.fulfill({json:{base64:"YWJj",eof:true,next:"3"}}));
 await page.getByText("Capture metadata cap-0",{exact:true}).click();const detail=page.locator("#artifact-cap-0");
 await expect(detail).toContainText("guest:p1");await expect(detail).toContainText("tap0");await expect(detail).toContainText("1 · Ethernet");await expect(detail).toContainText("60 / 90");await expect(detail).toContainText("2026-09-23T12:00:00Z");await expect(detail).toContainText("excluding PCAPNG overhead");
 await page.getByText("Capture metadata legacy-0",{exact:true}).click();await expect(page.locator("#artifact-legacy-0")).toContainText("Unavailable / Unavailable");
 await detail.getByRole("button",{name:"Download PCAPNG (3 bytes)",exact:true}).click();await expect(page.getByRole("alert")).toContainText("checksum mismatch");
 await detail.screenshot({path:resolve("../../docs/validation/inspectors-capture.png")});
});

test("network inspector separates stale evidence, unknown state and failure hypotheses",async({page})=>{
 await setup(page);
 await page.route("**/telemetry/query",r=>r.fulfill({json:{items:[],current:{link:{...sample,stale:true,adminUp:false,carrierUp:true,reason:"stats64_unavailable",mapping:{name:"tap7",ifindex:7},rstp:{},rstpObservedMonotonicNs:"123456"}}}}));
 await page.getByText("Accessible inventory · all nodes and data edges").click();await page.getByRole("button",{name:/^link: guest/}).click();
 const evidence=page.getByRole("region",{name:"Network observation evidence"});await expect(evidence).toContainText("Down / Up");await expect(evidence).toContainText("may be stale");await expect(evidence).toContainText("stats64_unavailable");await expect(evidence).toContainText("root cause: undetermined");await expect(evidence).toContainText("Unknown");
 await evidence.getByText("Observed runtime interface mapping",{exact:true}).click();await expect(evidence).toContainText("tap7");await evidence.getByText("Observed RSTP ports",{exact:true}).click();await expect(evidence).toContainText("RSTP unavailable or not applicable");
 await evidence.screenshot({path:resolve("../../docs/validation/inspectors-network.png")});
});

test("message observations freeze pages and distinguish exact, ambiguous and unavailable correlation",async({page})=>{
 await setup(page);let queries=0,status='exact';
 const event=(id:string)=>({id,observation:{kind:'send',stream:'alpha',phase:'request',sequence:id,payloadLength:'53',messageId:'a'.repeat(48),traceId:'a'.repeat(48),timestampMonotonicNs:'1000'}});
 await page.route('**/messages/query',async route=>{queries++;const p=route.request().postDataJSON();expect(p.node).toBe('guest');await route.fulfill({json:{items:[event(p.cursor?'1':'2')],nextCursor:p.cursor?null:'older',sources:[{epoch:'e',instance:'i',stale:true,missedBeforeIngestion:'12',reporterEvicted:'20',expiredOrPruned:'4',retained:'2'}]}});});
 await page.route('**/messages/correlate',async route=>{expect(route.request().postDataJSON().node).toBe('guest');await route.fulfill({json:{status,searchComplete:status==='exact',captureCoverage:'closed',reason:status==='exact'?'Exact identifier in verified UDP payload':status==='ambiguous'?'Multiple occurrences or incomplete coverage':'No verified match; no loss inferred',errors:[],matches:status==='unavailable'?[]:[{artifactId:'cap-0',edge:'link',packetIndex:'7',blockOffset:'128',matchBasis:'GLM1/v1 exact identifier'}]}});});
 await page.getByText('Accessible inventory · all nodes and data edges').click();await page.getByRole('button',{name:'guest · qemu · runtime ready',exact:true}).click();const panel=page.getByRole('region',{name:'Application message observations'});
 await expect(panel).toContainText('12 events missed before ingestion');await panel.getByRole('button',{name:'Older message observations',exact:true}).click();await expect(panel).toContainText('Frozen older message page');await expect(panel.getByRole('button',{name:'Correlate event 1',exact:true})).toBeVisible();const frozen=queries;await page.waitForTimeout(3300);expect(queries).toBe(frozen);
 await panel.getByRole('button',{name:'Correlate event 1',exact:true}).click();await expect(panel.getByRole('heading',{name:'Correlation: exact',exact:true})).toBeVisible();await expect(panel).toContainText('packet #7 · block offset 128');
 status='ambiguous';await panel.getByRole('button',{name:'Correlate event 1',exact:true}).click();await expect(panel.getByRole('heading',{name:'Correlation: ambiguous',exact:true})).toBeVisible();await panel.screenshot({path:resolve('../../docs/validation/messages-browser.png')});
 status='unavailable';await panel.getByRole('button',{name:'Correlate event 1',exact:true}).click();await expect(panel).toContainText('Correlation: unavailable');await expect(panel.getByRole('button',{name:'Show message capture cap-0',exact:true})).toHaveCount(0);await panel.getByRole('button',{name:'Return to newest messages',exact:true}).click();await expect(panel.getByRole('button',{name:'Correlate event 2',exact:true})).toBeVisible();await expect(panel).toContainText('Delivery accounting and loss: unavailable');
});
