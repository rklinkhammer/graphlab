import React, { useEffect, useState } from "react";
import { formatRate } from "./metrics";
type Report = {
  edge?: string;
  endpoint?: string;
  stream: string;
  epoch: string;
  sequence: string;
  counters: Record<string, string | null>;
};
type Sample = {
  node: string;
  observedAt: string;
  stale?: boolean;
  gapReason?: string | null;
  report: Report;
  rates: Record<string, number> | null;
  latency: {
    kind: string;
    count: string;
    meanUs: number | null;
    p95UpperBoundUs: number | null;
    p95Overflow: boolean;
  } | null;
};
type Response = {
  current: Sample[];
  items: Sample[];
  errors: Record<string, string>;
  truncated: boolean;
  observationProfile: string;
  runState: string;
};
export function ApplicationTelemetry({
  runId,
  node,
  edge = "",
  csrf,
  api,
}: {
  runId: string;
  node: string;
  edge?: string;
  csrf: string;
  api: (p: string, i?: RequestInit) => Promise<any>;
}) {
  const [data, setData] = useState<Response | null>(null),
    [error, setError] = useState(""),
    [observed, setObserved] = useState(0),
    [clock, setClock] = useState(performance.now());
  useEffect(() => {
    let live = true,
      busy = false;
    setData(null);
    setError("");
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const next = await api(`runs/${runId}/application-telemetry/query`, {
          method: "POST",
          headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
          body: JSON.stringify({ limit: 100, ...(edge ? {edge} : node ? { node } : {}) }),
        });
        if (live) {
          setData(next);
          setObserved(performance.now());
          setError("");
        }
      } catch (e) {
        if (live) setError((e as Error).message);
      } finally {
        busy = false;
      }
    }
    void refresh();
    const timer = setInterval(() => {
      setClock(performance.now());
      void refresh();
    }, 5000);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [runId, node, edge, csrf, api]);
  return (
    <section aria-label="Application telemetry">
      <h3>Application telemetry</h3>
      <p>
        Workload-reported messages and payload bytes. Separate from interface
        packets and wire bytes; no inferred link or message-to-capture mapping.
      </p>
      {edge && <p>Selected application edge {edge}. Source and target reports remain separate; no endpoint totals are summed.</p>}
      {error && (
        <p role="alert">Application telemetry unavailable or stale: {error}</p>
      )}
      {!data && !error && <p>Loading application observations…</p>}
      {data?.observationProfile === "minimal" && (
        <p>
          Application collection disabled by the minimal observation profile.
        </p>
      )}
      {data && !data.current.length && (
        <p>
          No application telemetry reported{node ? ` by ${node}` : ""}.
          Uninstrumented workloads remain supported; missing measurements are
          not zero.
        </p>
      )}
      {Object.entries(data?.errors ?? {}).map(([key, value]) => (
        <p role="alert" key={key}>
          {key}: {value}
        </p>
      ))}
      {data?.current.map((s) => {
        const stale = s.stale || Boolean(error) || clock - observed >= 15000;
        return (
          <article key={`${s.node}/${s.report.edge ?? "node"}/${s.report.endpoint ?? ""}`}>
            <h4>
              {s.node} · {s.report.stream}{s.report.edge ? ` · edge ${s.report.edge} · ${s.report.endpoint} reporter` : " · node-scoped report"}
            </h4>
            <p>
              {stale
                ? "Stale / retained observation"
                : "Current workload report"}{" "}
              · {s.observedAt} · {s.gapReason ?? "valid rate interval"}
            </p>
            <div className="metric-grid">
              {(s.report.endpoint ? [s.report.endpoint === "source" ? "sent" : "received"] : ["sent", "received"]).map((direction) => (
                <div key={direction}>
                  <strong>{direction === "sent" ? "Sent" : "Received"}</strong>
                  <p>
                    {stale
                      ? "—"
                      : formatRate(
                          s.rates?.[direction + "MessagesPerSecond"],
                          "msg/s",
                        )}
                  </p>
                  <p>
                    {stale
                      ? "—"
                      : formatRate(
                          s.rates?.[direction + "PayloadBytesPerSecond"],
                          "payload B/s",
                        )}
                  </p>
                  <p>
                    {s.report.counters[direction + "Messages"]} messages ·{" "}
                    {s.report.counters[direction + "PayloadBytes"]} payload
                    bytes (cumulative)
                  </p>
                </div>
              ))}
            </div>
            <p>
              Errors {s.report.counters.errors} · rejected messages{" "}
              {s.report.counters.rejectedMessages} · backpressure events{" "}
              {s.report.counters.backpressureEvents ?? "unavailable"}. These are application
              counters, not delivered network loss.
            </p>
            {s.report.edge && <p>Reconnects: {s.report.counters.reconnects ?? "unavailable"} · backpressure duration: {s.report.counters.backpressureNs == null ? "unavailable" : `${s.report.counters.backpressureNs} ns`}</p>}
            {s.latency ? (
              <>
                <h4>Local echo-service time</h4>
                <p>
                  Mean:{" "}
                  {stale || s.latency.meanUs == null
                    ? "—"
                    : `${s.latency.meanUs.toFixed(3)} µs`}{" "}
                  · p95 bucket upper bound:{" "}
                  {stale
                    ? "—"
                    : s.latency.p95Overflow
                      ? "> 10,000 µs (overflow)"
                      : s.latency.p95UpperBoundUs == null
                        ? "—"
                        : `${s.latency.p95UpperBoundUs} µs`}{" "}
                  · {s.latency.count} cumulative samples
                </p>
                <p>
                  Measured on one process monotonic clock, from completed
                  receive to successful echo send. Includes local processing and
                  send syscall time; excludes network transit and receive wait.
                  Not RTT or one-way network latency. No synchronized host
                  clocks required.
                </p>
              </>
            ) : (
              <p>Latency unavailable: no latency histogram reported.</p>
            )}
            <details>
              <summary>Report identity and exact counters</summary>
              <pre>{JSON.stringify(s, null, 2)}</pre>
            </details>
          </article>
        );
      })}
      <details>
        <summary>
          Application report history ({data?.items.length ?? 0})
        </summary>
        <p>
          Latest 100 returned reports · 24-hour retention · shared 10,000-report
          history ceiling.{" "}
          {data?.truncated
            ? "Query truncated; select a node to narrow the history."
            : ""}
        </p>
        <table>
          <thead>
            <tr>
              <th>Observed UTC</th>
              <th>Node / stream</th>
              <th>Sent / received messages</th>
              <th>Rate gap</th>
            </tr>
          </thead>
          <tbody>
            {data?.items.map((s, i) => (
              <tr key={i}>
                <td>{s.observedAt}</td>
                <td>
                  {s.node} / {s.report.stream}
                </td>
                <td>
                  {s.report.counters.sentMessages} /{" "}
                  {s.report.counters.receivedMessages}
                </td>
                <td>{s.gapReason ?? "—"}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </details>
    </section>
  );
}
