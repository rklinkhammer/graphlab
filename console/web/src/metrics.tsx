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
