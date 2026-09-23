import React, { useEffect, useState, useRef } from "react";
import { RateChart } from "./rate-chart";
import { HistoryPanel } from "./history";
import { formatRate, MetricDetails } from "./metrics";
type Api = (p: string, i?: RequestInit) => Promise<any>;
export function Telemetry({
  run,
  csrf,
  api,
  selectedEdge,
  onEdge,
  onRates,
}: {
  selectedEdge: string;
  onEdge: (id: string) => void;
  onRates: (v: Record<string, string>) => void;
  run: { id: string; state: string; revision: string };
  csrf: string;
  api: Api;
}) {
  const [data, setData] = useState<any>({ items: [], current: {} }),
    [faults, setFaults] = useState<any>({ items: {} }),
    [events, setEvents] = useState<any[]>([]),
    [direction, setDirection] = useState("a-to-b"),
    [delay, setDelay] = useState(100),
    [loss, setLoss] = useState(0),
    [duration, setDuration] = useState(10),
    [preview, setPreview] = useState<any>(null),
    [error, setError] = useState(""),
    [job, setJob] = useState<any>(null),
    [jobId, setJobId] = useState("");
  const edge = selectedEdge,
    setEdge = onEdge;
  const [resolution, setResolution] = useState(1);
  const [pollErrors, setPollErrors] = useState<Record<string, string>>({});
  const [observed, setObserved] = useState(0),
    [clock, setClock] = useState(performance.now());
  const stale = (s: any) => s.stale || clock - observed >= 3000;
  const post = (path: string, body: any) =>
    api(`runs/${run.id}/${path}`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
      body: JSON.stringify(body),
    });
  useEffect(() => {
    setPreview(null);
    setJobId("");
  }, [run.id]);
  useEffect(() => {
    let live = true,
      busy = false;
    const updateError = (key: string, message: string) => {
      if (live) setPollErrors((old) => ({ ...old, [key]: message }));
    };
    async function refresh() {
      if (busy) return;
      busy = true;
      const tasks = [
        post("telemetry/query", {
          resolutionSeconds: resolution,
          windowSeconds:
            resolution === 1 ? 3600 : resolution === 10 ? 86400 : 604800,
          limit: 2000,
          ...(edge ? { edge } : {}),
        })
          .then((t) => {
            if (live) {
              setData(t);
              setObserved(performance.now());
            }
            updateError("history", "");
          })
          .catch((e) => updateError("history", e.message)),
        api(`runs/${run.id}/faults`)
          .then((f) => {
            if (live) setFaults(f);
            updateError("faults", "");
          })
          .catch((e) => updateError("faults", e.message)),
        api(`runs/${run.id}/timeline`)
          .then((e) => {
            if (live) setEvents(e.items);
            updateError("timeline", "");
          })
          .catch((e) => updateError("timeline", e.message)),
      ];
      await Promise.allSettled(tasks);
      busy = false;
    }
    void refresh();
    const timer = setInterval(() => {
      setClock(performance.now());
      void refresh();
    }, 1000);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [run.id, api, resolution, edge]);

  useEffect(() => {
    if (!jobId) return;
    let live = true;
    const refresh = () =>
      void api(`jobs/${jobId}`)
        .then((j) => {
          if (live) setJob(j);
        })
        .catch((e) => setError(e.message));
    refresh();
    const timer = setInterval(refresh, 500);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [jobId, api]);
  useEffect(() => {
    onRates(
      Object.fromEntries(
        Object.entries(data.current ?? {}).map(([id, s]: [string, any]) => [
          id,
          stale(s)
            ? "stale"
            : `A→B ${formatRate(s.forwardBitsPerSecond)} · B→A ${formatRate(s.reverseBitsPerSecond)}`,
        ]),
      ),
    );
  }, [data, clock, observed, onRates]);
  useEffect(() => () => onRates({}), [onRates]);
  const spec = {
    edge,
    direction,
    kind: "netem",
    delayMs: delay,
    lossPercent: loss,
    durationSeconds: duration,
  };
  const signature = JSON.stringify(spec);
  const currentSignature = useRef(signature);
  currentSignature.current = signature;
  useEffect(() => setPreview(null), [signature]);
  async function action(remove?: string) {
    try {
      const result = await post(remove ? "faults/remove" : "faults", {
        ...(remove ? { faultId: remove } : { fault: spec }),
        expectedRevision: faults.revision ?? run.revision,
        idempotencyKey: crypto.randomUUID(),
      });
      setJobId(result.jobId);
      setPreview(null);
    } catch (e) {
      setError((e as Error).message);
    }
  }
  const rows = Object.values({
    ...Object.fromEntries(
      (data.items ?? []).map((s: any) => [s.edge, { ...s, stale: true }]),
    ),
    ...(data.current ?? {}),
  }) as any[];
  const selected = rows.find((s) => s.edge === edge);
  const history = (data.items ?? []).filter((s: any) => s.edge === edge);
  const max = Math.max(
    1,
    ...history.flatMap((s: any) => [
      s.forwardBitsPerSecond ?? 0,
      s.reverseBitsPerSecond ?? 0,
    ]),
  );
  return (
    <section id="link-performance" aria-label="Telemetry and faults">
      <h3>Directional telemetry and faults</h3>
      <p>
        One counter source per edge. Software interface rates are not
        physical-wire utilization. A→B follows the topology's ordered endpoints.
        Gaps and first samples have no rate.
      </p>
      <label>
        History{" "}
        <select
          aria-label="Telemetry history resolution"
          value={resolution}
          onChange={(e) => setResolution(Number(e.target.value))}
        >
          <option value={1}>1 second · last hour</option>
          <option value={10}>10 seconds · last day</option>
          <option value={60}>1 minute · last week</option>
        </select>
      </label>
      <table>
        <thead>
          <tr>
            <th>Edge</th>
            <th>A→B bit/s</th>
            <th>B→A bit/s</th>
            <th>Observation</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((s) => (
            <tr key={s.edge}>
              <td>
                <button onClick={() => setEdge(s.edge)}>{s.edge}</button>
              </td>
              <td>
                {stale(s) ? "—" : (s.forwardBitsPerSecond?.toFixed(0) ?? "—")}
              </td>
              <td>
                {stale(s) ? "—" : (s.reverseBitsPerSecond?.toFixed(0) ?? "—")}
              </td>
              <td>
                {stale(s) ? "stale" : (s.gapReason ?? "current")} · admin{" "}
                {String(s.adminUp ?? "unknown")} · carrier{" "}
                {String(s.carrierUp ?? "unknown")}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {edge && (
        <>
          <MetricDetails
            sample={selected}
            stale={Boolean(selected && stale(selected))}
          />
          <h4>
            {edge} · {selected?.endpoints?.join(" → ")}
          </h4>
          <RateChart points={history} resolution={resolution} edge={edge} />
          <details>
            <summary>Inspect chart values</summary>
            <table>
              <thead>
                <tr>
                  <th>UTC</th>
                  <th>A→B bit/s</th>
                  <th>B→A bit/s</th>
                  <th>Gaps</th>
                </tr>
              </thead>
              <tbody>
                {history.slice(-200).map((s: any, i: number) => (
                  <tr key={i}>
                    <td>
                      {new Date(
                        Number(s.bucketUnixSeconds) * 1000,
                      ).toISOString()}
                    </td>
                    <td>{s.forwardBitsPerSecond ?? "—"}</td>
                    <td>{s.reverseBitsPerSecond ?? "—"}</td>
                    <td>{s.gapCount ?? 0}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <p>Latest 200 returned points.</p>
          </details>
          <p>
            Solid: A→B; dashed: B→A. Peak scale {max.toFixed(0)} bit/s.{" "}
            {data.truncated ? "History truncated by point limit." : ""} Oldest
            retained bucket:{" "}
            {data.oldestAvailableUnixSeconds
              ? new Date(
                  Number(data.oldestAvailableUnixSeconds) * 1000,
                ).toISOString()
              : "none"}
            . Retention is also limited by a shared 100,000-row capacity.
          </p>
          <details>
            <summary>Counter source, mapping, qdisc and epoch</summary>
            <pre>{JSON.stringify(selected, null, 2)}</pre>
          </details>
        </>
      )}
      <form
        onSubmit={(e) => {
          e.preventDefault();
          void post("faults/preview", { fault: spec })
            .then((result) => {
              if (currentSignature.current === signature)
                setPreview({ signature, result });
            })
            .catch((e) => setError(e.message));
        }}
      >
        <label>
          Fault edge{" "}
          <select
            aria-label="Fault edge"
            value={edge}
            onChange={(e) => setEdge(e.target.value)}
          >
            <option value="">Select edge</option>
            {rows.map((s) => (
              <option key={s.edge}>{s.edge}</option>
            ))}
          </select>
        </label>
        <label>
          Direction{" "}
          <select
            aria-label="Fault direction"
            value={direction}
            onChange={(e) => setDirection(e.target.value)}
          >
            <option value="a-to-b">A→B</option>
            <option value="b-to-a">B→A</option>
          </select>
        </label>
        <label>
          Delay ms{" "}
          <input
            type="number"
            min="0"
            max="5000"
            value={delay}
            onChange={(e) => setDelay(Number(e.target.value))}
          />
        </label>
        <label>
          Loss percent{" "}
          <input
            type="number"
            min="0"
            max="100"
            value={loss}
            onChange={(e) => setLoss(Number(e.target.value))}
          />
        </label>
        <label>
          Duration seconds{" "}
          <input
            type="number"
            min="1"
            max="3600"
            value={duration}
            onChange={(e) => setDuration(Number(e.target.value))}
          />
        </label>
        <button disabled={!edge || !["ready", "stopped"].includes(run.state)}>
          Preview fault placement
        </button>
      </form>
      {preview?.signature === signature && (
        <div>
          <pre>{JSON.stringify(preview.result, null, 2)}</pre>
          <button onClick={() => void action()}>Apply directional fault</button>
        </div>
      )}
      {job && (
        <p role="status">
          Fault job: {job.state}
          {job.error ? ` · ${job.error}` : ""}
          {["queued", "running"].includes(job.state) &&
            job.operation === "fault.apply" && (
              <button
                onClick={() =>
                  void api(`jobs/${jobId}/cancel`, {
                    method: "POST",
                    headers: {
                      "Content-Type": "application/json",
                      "X-CSRF-Token": csrf,
                    },
                    body: "{}",
                  }).catch((e) => setError(e.message))
                }
              >
                Cancel fault job
              </button>
            )}
        </p>
      )}
      <ul>
        {Object.values(faults.items ?? {}).map((f: any) => (
          <li key={f.id}>
            {f.edge} {f.direction} · {f.delayMs} ms / {f.lossPercent}% ·{" "}
            {f.state} · {f.durationSeconds}s{" "}
            {f.state === "active" && (
              <button onClick={() => void action(f.id)}>Remove fault</button>
            )}
          </li>
        ))}
      </ul>
      {data.collectorError && (
        <p role="alert">Collector: {data.collectorError}</p>
      )}
      {Object.entries(pollErrors)
        .filter(([, v]) => v)
        .map(([k, v]) => (
          <p role="alert" key={k}>
            {k}: {v}
          </p>
        ))}
      {error && <p role="alert">{error}</p>}
      <details id="run-history">
        <summary>Correlated timeline ({events.length})</summary>
        <HistoryPanel events={events} />
      </details>
    </section>
  );
}
