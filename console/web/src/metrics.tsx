import React, { useState } from "react";
export function formatRate(value: number | null | undefined, unit = "bit/s") {
  if (value == null || !Number.isFinite(value)) return "—";
  const scale =
    value >= 1e9 ? 1e9 : value >= 1e6 ? 1e6 : value >= 1e3 ? 1e3 : 1;
  return `${(value / scale).toLocaleString(undefined, { maximumFractionDigits: 2 })} ${scale === 1e9 ? "G" : scale === 1e6 ? "M" : scale === 1e3 ? "k" : ""}${unit}`;
}
export type Sample = {
  edge: string;
  stale?: boolean;
  valid?: boolean;
  forwardMetric?: string;
  forwardBitsPerSecond?: number | null;
  reverseBitsPerSecond?: number | null;
  forwardPacketsPerSecond?: number | null;
  reversePacketsPerSecond?: number | null;
  raw?: Record<string, string>;
  mappingEpoch?: string;
  counterEpoch?: string;
  source?: string;
  at?: string;
  adminUp?: boolean;
  carrierUp?: boolean;
  gapReason?: string;
  mapping?: Record<string, unknown>;
  rstp?: Record<string, unknown>;
  rstpObservedMonotonicNs?: string;
  operstate?: string;
  reason?: string;
};
export function MetricDetails({
  sample,
  stale,
}: {
  sample?: Sample;
  stale: boolean;
}) {
  const [baseline, setBaseline] = useState<{
    edge: string;
    epoch?: string;
    counterEpoch?: string;
    raw: Record<string, string>;
  } | null>(null);
  const usable =
    baseline?.edge === sample?.edge &&
    baseline?.epoch === sample?.mappingEpoch &&
    baseline?.counterEpoch === sample?.counterEpoch;
  const raw = (key: string) => {
    const v = sample?.raw?.[key];
    if (v == null) return "—";
    if (!usable) return v;
    try {
      const d = BigInt(v) - BigInt(baseline!.raw[key]);
      return d < 0n ? "— (counter reset)" : d.toString();
    } catch {
      return "—";
    }
  };
  const forward = sample?.forwardMetric === "rx" ? "rx" : "tx",
    reverse = forward === "rx" ? "tx" : "rx";
  return (
    <section aria-label="Link performance">
      <h3>Link performance</h3>
      <p>
        {stale
          ? "Stale observation"
          : (sample?.gapReason ??
            (sample ? "Latest observation" : "No observation available"))}{" "}
        · {sample?.source ?? "Source unavailable"} ·{" "}
        {sample?.at ?? "Observation time unavailable"}
      </p>
      <section aria-label="Network observation evidence">
        <h4>Network observation evidence</h4>
        <p>Values below belong to the observation above{stale ? " and may be stale" : ""}. Administrative state, carrier and RSTP are independent observations; none proves application reachability or delivery.</p>
        <dl><dt>Administrative / carrier state</dt><dd>{sample?.adminUp == null ? "Unknown" : sample.adminUp ? "Up" : "Down"} / {sample?.carrierUp == null ? "Unknown" : sample.carrierUp ? "Up" : "Down"}</dd>
        <dt>Operational state</dt><dd>{sample?.operstate ?? "Unknown"}</dd>
        <dt>Mapping identity digest</dt><dd>{sample?.mappingEpoch ?? "Unavailable"}</dd>
        <dt>Counter epoch</dt><dd>{sample?.counterEpoch ?? "Unavailable"}</dd>
        <dt>Collection failure evidence</dt><dd>{sample?.reason ?? "No failure reason reported; this is not a health assertion."}</dd>
        <dt>RSTP cache observation (host monotonic ns)</dt><dd>{sample?.rstpObservedMonotonicNs ?? "Unavailable"}</dd></dl>
        <p>RSTP comes from a separately cached OVS observation. Its monotonic clock is host-local; it is not a wall-clock timestamp or evidence of an observed route.</p>
        <details><summary>Observed runtime interface mapping</summary><pre>{sample?.mapping ? JSON.stringify(sample.mapping,null,2) : "Mapping unavailable"}</pre></details>
        <details><summary>Observed RSTP ports</summary><pre>{sample?.rstp && Object.keys(sample.rstp).length ? JSON.stringify(sample.rstp,null,2) : "RSTP unavailable or not applicable; no forwarding state inferred"}</pre></details>
        <p>Failure layer and root cause: undetermined. Inspect the reported observations; missing telemetry is not proof of a network failure.</p>
      </section>
      <div className="metric-grid">
        {[
          ["A→B", forward, "forward"],
          ["B→A", reverse, "reverse"],
        ].map(([label, prefix, direction]) => (
          <div key={label}>
            <strong>{label}</strong>
            <p>
              {stale
                ? "—"
                : formatRate(
                    sample?.[`${direction}BitsPerSecond` as keyof Sample] as
                      | number
                      | undefined,
                  )}
            </p>
            <p>
              {stale
                ? "—"
                : formatRate(
                    sample?.[`${direction}PacketsPerSecond` as keyof Sample] as
                      | number
                      | undefined,
                    "pkt/s",
                  )}
            </p>
            <dl>
              {["Bytes", "Packets", "Errors", "Dropped"].map((field) => (
                <React.Fragment key={field}>
                  <dt>{field}</dt>
                  <dd>{raw(prefix + field)}</dd>
                </React.Fragment>
              ))}
            </dl>
          </div>
        ))}
      </div>
      <button
        disabled={!sample?.raw || stale}
        onClick={() =>
          setBaseline({
            edge: sample!.edge,
            epoch: sample!.mappingEpoch,
            counterEpoch: sample!.counterEpoch,
            raw: { ...sample!.raw },
          })
        }
      >
        Set display counter baseline
      </button>
      <button disabled={!baseline} onClick={() => setBaseline(null)}>
        Clear display baseline
      </button>
      <p>
        {usable
          ? "Counters since display baseline."
          : "Cumulative interface counters."}{" "}
        Retained data and host counters are unchanged. Counters may be old when
        observation is stale.
      </p>
      <p>
        Mean/p95 latency, jitter, delivered loss, application messages,
        reconnects and backpressure: unavailable from interface counters. See
        Application telemetry for separately reported workload measurements.
        Interface drops are not end-to-end packet loss.
      </p>
    </section>
  );
}
