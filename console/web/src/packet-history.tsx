import React, { useEffect, useState } from "react";

export function PacketHistory({runId, node, edge, csrf, api, onArtifact}: {
  runId: string; node: string; edge: string; csrf: string;
  api: (path: string, options?: RequestInit) => Promise<any>;
  onArtifact: (id: string, edge: string) => void;
}) {
  const [data, setData] = useState<any>(null), [error, setError] = useState("");
  const [cursor, setCursor] = useState<string | null>(null), [protocol, setProtocol] = useState("");
  const [revision, setRevision] = useState(0);
  const selection = `${runId}/${node}/${edge}/${protocol}`;
  const [scope, setScope] = useState(selection);
  // Reset pagination immediately when graph/run/filter selection changes.
  if (scope !== selection) { setScope(selection); setCursor(null); setData(null); setError(""); }
  useEffect(() => {
    let live = true, busy = false;
    setData(null); setError("");
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const next = await api(`runs/${runId}/packet-history/query`, {
          method: "POST", headers: {"Content-Type": "application/json", "X-CSRF-Token": csrf},
          body: JSON.stringify({limit: 50, ...(node ? {node} : {}), ...(edge ? {edge} : {}), ...(protocol ? {protocol} : {}), ...(cursor ? {cursor} : {})}),
        });
        if (live) { setData(next); setError(""); }
      } catch (e) { if (live) setError((e as Error).message); }
      finally { busy = false; }
    }
    void refresh();
    const timer = cursor ? undefined : setInterval(refresh, 5000);
    return () => { live = false; if (timer) clearInterval(timer); };
  }, [runId, node, edge, protocol, cursor, csrf, api, revision]);
  return <section aria-label="Packet history">
    <h3>Packet history</h3>
    <p>Finalized, checksum-verified capture segments only. Active and unmanifested files are excluded. Packets are not application messages; direction and delivery loss are not inferred.</p>
    <p>Selected {edge ? `edge ${edge}` : node ? `node ${node} (incident capture edges)` : "run"} · newest indexed first, not timestamp order.</p>
    <label>Packet protocol <select value={protocol} onChange={e => setProtocol(e.target.value)}>
      <option value="">All protocols</option>{["udp", "tcp", "arp", "icmp", "icmpv6", "ipv4", "ipv6", "other"].map(p => <option key={p}>{p}</option>)}
    </select></label>{" "}
    <button onClick={() => {setCursor(null); setRevision(v => v+1);}}>Return to newest packets</button>{" "}
    <button disabled={!data?.nextCursor || Boolean(error)} onClick={() => setCursor(data.nextCursor)}>Older packets</button>
    <p>{cursor ? "Older page frozen; new observations do not replace this page." : "Following newest indexed packets; refresh every 5 seconds."}</p>
    {error && <p role="alert">Packet history unavailable: {error}. If retention expired, return to newest packets.</p>}
    {data?.indexError && <p role="alert">Indexing incomplete: {data.indexError}</p>}
    {!data && !error && <p>Loading packet index…</p>}
    {data && !data.items?.length && <p>No retained packets match. Captures may be disabled, still active, awaiting indexing, empty, expired, or excluded by limits.</p>}
    {data && <p>Capture coverage: {data.captureCoverage} · {data.indexing ? "index scan queued or active" : "index idle"}. 24-hour index retention; 20,000 records / 16 MiB of record bodies globally; first 2,000 packets per segment. Capture downloads retain their independent lifecycle.</p>}
    {data && <p>Globally retained: {data.retainedRecords ?? "unknown"} records / {data.retainedRecordBytes ?? "unknown"} encoded bytes · retention generation {data.generation ?? "unknown"}.</p>}
    <div style={{overflow: "auto", maxHeight: 560}}><table>
      <thead><tr><th>Capture time (Unix µs)</th><th>Edge / interface</th><th>Protocol / endpoints</th><th>Lengths</th><th>Capture reference</th></tr></thead>
      <tbody>{data?.items?.map((p: any) => <tr key={p.id}>
        <td>{p.timestampUnixMicros}<br/>Host realtime; µs resolution</td>
        <td>{p.edge} / {p.interface}<br/>Direction: {p.direction}</td>
        <td>{p.headers.protocol} · {p.headers.decodeStatus}<br/>{p.headers.sourceAddress ?? "—"}{p.headers.sourcePort != null ? `:${p.headers.sourcePort}` : ""} → {p.headers.destinationAddress ?? "—"}{p.headers.destinationPort != null ? `:${p.headers.destinationPort}` : ""}</td>
        <td>{p.capturedLength} / {p.originalLength} bytes{p.truncated ? " · truncated" : ""}</td>
        <td><button onClick={() => onArtifact(p.artifactId, p.edge)}>Show capture {p.artifactId}</button><br/>Packet #{p.packetIndex} · block offset {p.blockOffset}<details><summary>Exact observation</summary><pre>{JSON.stringify(p,null,2)}</pre></details></td>
      </tr>)}</tbody>
    </table></div>
    <details><summary>Segment indexing and omissions ({data?.segments?.length ?? 0})</summary>
      <p>Counts describe initial indexing. Retention can subsequently remove rows. Failed segments have no indexed packets; source artifacts are unchanged.</p>
      <ul>{data?.segments?.map((s: any) => <li key={s.artifactId}>{s.artifactId} · {s.state} · indexed {s.indexedRecords ?? "0"} · omitted {s.omittedRecords ?? "unknown"}{s.error ? ` · ${s.error}` : ""}</li>)}</ul>
    </details>
  </section>;
}
