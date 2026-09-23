import React from "react";
type Point = {
  bucketUnixSeconds: string;
  forwardBitsPerSecond?: number | null;
  reverseBitsPerSecond?: number | null;
  gapCount?: number;
};
export function RateChart({
  points,
  resolution,
  edge,
}: {
  points: Point[];
  resolution: number;
  edge: string;
}) {
  const max = Math.max(
    1,
    ...points.flatMap((s) => [
      s.forwardBitsPerSecond ?? 0,
      s.reverseBitsPerSecond ?? 0,
    ]),
  );
  const start = Number(points[0]?.bucketUnixSeconds ?? 0),
    end = Number(points.at(-1)?.bucketUnixSeconds ?? start);
  const x = (t: number) => 60 + ((t - start) * 470) / Math.max(1, end - start),
    y = (v: number) => 140 - (v / max) * 110;
  function paths(key: "forwardBitsPerSecond" | "reverseBitsPerSecond") {
    const segments: string[] = [];
    let line = "",
      previous: number | undefined;
    for (const p of points) {
      const t = Number(p.bucketUnixSeconds);
      if (previous !== undefined && t - previous > resolution * 1.5) {
        if (line) segments.push(line);
        line = "";
      }
      previous = t;
      const v = p[key];
      if (v == null || p.gapCount) {
        if (line) segments.push(line);
        line = "";
        continue;
      }
      line += `${line ? "L" : "M"}${x(t)},${y(v)} `;
    }
    if (line) segments.push(line);
    return segments;
  }
  const time = (t: number) => new Date(t * 1000).toISOString().slice(11, 19);
  return (
    <svg
      viewBox="0 0 590 195"
      role="img"
      aria-label={`${edge} directional rate history; solid forward, dashed reverse`}
      style={{ width: "100%", maxWidth: 800, background: "#eef5f5" }}
    >
      <text x="60" y="16" fontSize="11">
        bit/s · solid A→B · dashed B→A
      </text>
      {[0, max / 2, max].map((v) => (
        <g key={v}>
          <line x1="60" x2="530" y1={y(v)} y2={y(v)} stroke="#cbd9de" />
          <text x="54" y={y(v) + 4} textAnchor="end" fontSize="10">
            {v.toLocaleString(undefined, { maximumFractionDigits: 2 })}
          </text>
        </g>
      ))}
      {(["forwardBitsPerSecond", "reverseBitsPerSecond"] as const).map(
        (key, i) => (
          <g key={key}>
            {paths(key).map((d, n) => (
              <path
                key={n}
                d={d}
                stroke={i ? "#923f17" : "#087f8c"}
                strokeDasharray={i ? "5 3" : undefined}
                fill="none"
              />
            ))}
            {points.map((p, n) =>
              p[key] != null && !p.gapCount ? (
                <circle
                  key={n}
                  cx={x(Number(p.bucketUnixSeconds))}
                  cy={y(p[key]!)}
                  r="2"
                  fill={i ? "#923f17" : "#087f8c"}
                >
                  <title>
                    {time(Number(p.bucketUnixSeconds))} UTC ·{" "}
                    {i ? "B→A" : "A→B"}: {p[key]} bit/s
                  </title>
                </circle>
              ) : null,
            )}
          </g>
        ),
      )}
      {points.length > 0 && (
        <>
          <text x="60" y="164" fontSize="11">
            {time(start)}
          </text>
          <text x="530" y="164" textAnchor="end" fontSize="11">
            {time(end)} UTC
          </text>
        </>
      )}
      <text x="60" y="184" fontSize="10">
        Missing samples break the line. Hover a point for its value.
      </text>
    </svg>
  );
}
