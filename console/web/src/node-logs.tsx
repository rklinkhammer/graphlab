import React, { useEffect, useRef, useState } from "react";
type Snapshot = {
  base64: string;
  source: string;
  generation: string;
  observedAt: string;
  truncated: boolean;
  limitBytes: number;
};
export function NodeLogs({
  runId,
  node,
  csrf,
  api,
}: {
  runId: string;
  node: string;
  csrf: string;
  api: (p: string, i?: RequestInit) => Promise<any>;
}) {
  const [data, setData] = useState<Snapshot | null>(null),
    [error, setError] = useState(""),
    [search, setSearch] = useState(""),
    [paused, setPaused] = useState(false),
    [gap, setGap] = useState("");
  const output = useRef<HTMLPreElement>(null),
    previous = useRef<Snapshot | null>(null);
  useEffect(() => {
    let live = true,
      busy = false;
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const next = await api(`runs/${runId}/terminal`, {
          method: "POST",
          headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
          body: JSON.stringify({ operation: "logs", node }),
        });
        if (live) {
          if (
            previous.current &&
            previous.current.generation !== next.generation
          )
            setGap(
              "Runtime generation changed; earlier output may be omitted.",
            );
          previous.current = next;
          setData(next);
          setError("");
        }
      } catch (e) {
        if (live) setError((e as Error).message);
      } finally {
        busy = false;
      }
    }
    void refresh();
    const timer = setInterval(refresh, 2500);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [runId, node, csrf, api]);
  useEffect(() => {
    if (!paused && output.current)
      output.current.scrollTop = output.current.scrollHeight;
  }, [data, paused]);
  const bytes = data
    ? Uint8Array.from(atob(data.base64), (c) => c.charCodeAt(0))
    : new Uint8Array();
  const text = new TextDecoder().decode(bytes);
  function download() {
    const url = URL.createObjectURL(
      new Blob([bytes], { type: "application/octet-stream" }),
    );
    const a = document.createElement("a");
    a.href = url;
    a.download = `${node}-process.log`;
    a.click();
    URL.revokeObjectURL(url);
  }
  return (
    <section aria-label="Node logs">
      <h4>{node} · Process logs</h4>
      <p>
        {data?.source ?? "Waiting for source"} ·{" "}
        {data?.observedAt ?? "Not observed"} · generation{" "}
        {data?.generation ?? "unknown"}
      </p>
      <p>
        Bounded to 200 lines and {data?.limitBytes ?? 65536} bytes. Earlier
        output may be omitted. {data?.truncated ? "Byte limit reached." : ""}{" "}
        Logs are separate from recorded terminal output.
      </p>
      <label>
        Search loaded output{" "}
        <input value={search} onChange={(e) => setSearch(e.target.value)} />
      </label>
      <button onClick={() => setPaused((p) => !p)}>
        {paused ? "Resume scrolling" : "Pause scrolling"}
      </button>
      <button disabled={!data} onClick={download}>
        Download retained output
      </button>
      {gap && <p role="note">{gap}</p>}
      {error && <p role="alert">Logs unavailable or stale: {error}</p>}
      <pre className="console-log" ref={output}>
        {search
          ? text
              .split("\n")
              .filter((line) => line.includes(search))
              .join("\n")
          : text || "No output returned."}
      </pre>
    </section>
  );
}
