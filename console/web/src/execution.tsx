import React, { useEffect, useState } from "react";
import { ApplicationTelemetry } from "./application-telemetry";
import { Telemetry } from "./telemetry";
import { RecordedTerminal } from "./terminal";
type Run = {
  id: string;
  state: string;
  revision: string;
  topologyHash: string;
  captureCoverage?: string;
};
type Job = {
  id: string;
  operation: string;
  state: string;
  error: string | null;
};
type Artifact = {
  id?: string;
  edge: string;
  epoch?: string;
  state: string;
  size?: string;
  sha256?: string;
  mediaType?: string;
};
type Api = (path: string, init?: RequestInit) => Promise<any>;
export function Execution({
  hash,
  csrf,
  api,
  required,
  selectedNode,
  selectedEdge,
  onEdge,
  onRates,
}: {
  hash: string;
  csrf: string;
  api: Api;
  required: boolean;
  selectedNode: string;
  selectedEdge: string;
  onEdge: (id: string) => void;
  onRates: (v: Record<string, string>) => void;
}) {
  const [retained, setRetained] = useState<Run | null>(null);
  const [runs, setRuns] = useState<Run[]>([]),
    [run, setRun] = useState<Run | null>(null),
    [job, setJob] = useState<Job | null>(null),
    [jobId, setJobId] = useState(""),
    [error, setError] = useState(""),
    [ack, setAck] = useState(false),
    [pending, setPending] = useState(false),
    [artifactRun, setArtifactRun] = useState(""),
    [artifacts, setArtifacts] = useState<Artifact[]>([]);
  useEffect(() => {
    let live = true,
      busy = false;
    setRetained(null);
    if (!artifactRun) return;
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const r = await api(`runs/${artifactRun}`);
        if (live && r.topologyHash === hash) setRetained(r);
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
  }, [artifactRun, api, hash]);
  useEffect(() => {
    let cancelled = false,
      busy = false;
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const list = await api("runs");
        if (cancelled) return;
        setRuns(list.items.filter((r: Run) => r.topologyHash === hash));
        const active = list.items.find(
          (r: Run) => r.topologyHash === hash && r.state !== "destroyed",
        );
        if (active) {
          const detail = await api(`runs/${active.id}`);
          if (!cancelled) setRun(detail);
        } else setRun(null);
        if (jobId) {
          const value = await api(`jobs/${jobId}`);
          if (!cancelled) setJob(value);
        }
      } catch (e) {
        if (!cancelled) setError((e as Error).message);
      } finally {
        busy = false;
      }
    }
    void refresh();
    const timer = setInterval(refresh, 1200);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [jobId, api, hash]);
  const catalogRun = artifactRun || run?.id || "";
  useEffect(() => {
    setArtifacts([]);
    if (!catalogRun) return;
    let cancelled = false,
      busy = false;
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const value = await api(`runs/${catalogRun}/artifacts`);
        if (!cancelled) setArtifacts(value.items);
      } catch (e) {
        if (!cancelled) setError((e as Error).message);
      } finally {
        busy = false;
      }
    }
    void refresh();
    const timer = setInterval(refresh, 2500);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [catalogRun, api]);
  async function submit(operation: string) {
    setPending(true);
    setError("");
    try {
      const key = crypto.randomUUID();
      let path = "runs",
        body: Record<string, unknown> = {
          topologyHash: hash,
          idempotencyKey: key,
          developmentMode: !required && ack,
        };
      if (operation !== "start") {
        if (!run) return;
        path = `runs/${run.id}/operations`;
        body = {
          operation,
          expectedRevision: run.revision,
          idempotencyKey: key,
        };
      }
      const value = await api(path, {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
        body: JSON.stringify(body),
      });
      setJobId(value.jobId);
      setArtifactRun(value.runId);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function download(a: Artifact) {
    if (!a.id) return;
    setPending(true);
    try {
      let offset = "0";
      const chunks: BlobPart[] = [];
      for (;;) {
        const result = await api(
          `runs/${catalogRun}/artifacts/${a.id}/chunks/${offset}`,
        );
        const bytes = Uint8Array.from(atob(result.base64), (c: string) =>
          c.charCodeAt(0),
        );
        chunks.push(bytes);
        if (result.eof) break;
        if (result.next === offset)
          throw new Error("Artifact transfer did not advance");
        offset = result.next;
      }
      const blob = new Blob(chunks, {
        type: a.mediaType ?? "application/x-pcapng",
      });
      const digest = Array.from(
        new Uint8Array(
          await crypto.subtle.digest("SHA-256", await blob.arrayBuffer()),
        ),
        (v) => v.toString(16).padStart(2, "0"),
      ).join("");
      if (`sha256:${digest}` !== a.sha256)
        throw new Error("Artifact checksum mismatch");
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = `${a.id}.${a.mediaType === "application/x-graphlab-terminal" ? "glterm" : "pcapng"}`;
      link.click();
      URL.revokeObjectURL(url);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function cancel() {
    if (!jobId) return;
    try {
      await api(`jobs/${jobId}/cancel`, {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
        body: "{}",
      });
    } catch (e) {
      setError((e as Error).message);
    }
  }
  const inspected = artifactRun
    ? artifactRun === run?.id
      ? run
      : retained
    : run;
  const inspectingOther = Boolean(artifactRun && artifactRun !== run?.id);
  const busy =
    inspectingOther ||
    pending ||
    job?.state === "queued" ||
    job?.state === "running";
  return (
    <section className="execution" aria-label="Run controls">
      <div>
        <strong>
          {required ? "M3 capture-first execution" : "M2 development execution"}
        </strong>
        <p>
          {required
            ? "Traffic releases after every data-edge capture is armed. A 10-second renewable traffic lease expires locally if the controller is unavailable. Drops and pre-activation traffic are not covered."
            : "Capture coverage is unavailable for this topology."}
        </p>
      </div>
      {!run ? (
        <>
          {!required && (
            <label>
              <input
                type="checkbox"
                checked={ack}
                onChange={(e) => setAck(e.target.checked)}
              />{" "}
              I accept running without capture coverage.
            </label>
          )}
          <button
            disabled={(!required && !ack) || busy || !hash}
            onClick={() => submit("start")}
          >
            Start selected topology
          </button>
        </>
      ) : (
        <>
          <p>
            Run {run.id} · <strong>{run.state}</strong> · coverage:{" "}
            {run.captureCoverage} · revision {run.revision}
          </p>
          <div className="actions">
            <button
              disabled={busy || run.state !== "ready"}
              onClick={() => submit("stop")}
            >
              Quiesce
            </button>
            <button
              disabled={busy || run.state !== "stopped"}
              onClick={() => submit("resume")}
            >
              Resume
            </button>
            <button
              disabled={busy}
              onClick={() =>
                submit(run.state === "reconciling" ? "recover" : "destroy")
              }
            >
              {run.state === "reconciling"
                ? "Recover and clean up"
                : "Destroy run"}
            </button>
          </div>
          <details>
            <summary>Runtime resource identities</summary>
            <pre>{JSON.stringify(run, null, 2)}</pre>
          </details>
        </>
      )}
      {job && (
        <p role="status">
          Job {job.operation}: {job.state}
          {job.error ? ` · ${job.error}` : ""}
          {busy && job.operation === "start" && (
            <button onClick={cancel}>Cancel start</button>
          )}
        </p>
      )}
      {error && <p role="alert">{error}</p>}
      <nav className="actions" aria-label="Run workspace">
        <button
          onClick={() =>
            document
              .getElementById("link-performance")
              ?.scrollIntoView({ behavior: "smooth" })
          }
        >
          Performance
        </button>
        <button
          onClick={() =>
            document
              .getElementById("node-consoles")
              ?.scrollIntoView({ behavior: "smooth" })
          }
        >
          Consoles
        </button>
        <button
          onClick={() => {
            const d = document.getElementById(
              "run-history",
            ) as HTMLDetailsElement | null;
            if (d) {
              d.open = true;
              d.scrollIntoView({ behavior: "smooth" });
            }
          }}
        >
          History
        </button>
        <button
          onClick={() =>
            document
              .getElementById("capture-catalog")
              ?.scrollIntoView({ behavior: "smooth" })
          }
        >
          Captures
        </button>
      </nav>
      {inspectingOther && (
        <p>
          Inspecting retained run {artifactRun}. Select the active run to enable
          its lifecycle controls.
        </p>
      )}
      <h3 id="capture-catalog">Capture and recording catalog</h3>
      <p>
        {selectedEdge
          ? `Filtered to edge ${selectedEdge} and terminal recordings.`
          : "All retained artifacts for the chosen run."}
      </p>
      <label>
        Run artifacts{" "}
        <select
          value={artifactRun}
          onChange={(e) => {
            setRetained(null);
            setArtifacts([]);
            setArtifactRun(e.target.value);
          }}
        >
          <option value="">Active run (or select a retained run)</option>
          {runs.map((r) => (
            <option key={r.id} value={r.id}>
              {r.id} · {r.state}
            </option>
          ))}
        </select>
      </label>
      <ul>
        {artifacts
          .filter(
            (a) =>
              !selectedEdge ||
              a.edge === selectedEdge ||
              a.mediaType === "application/x-graphlab-terminal",
          )
          .map((a, i) => (
            <li key={a.id ?? i}>
              {a.edge} · {a.mediaType ?? "application/x-pcapng"} ·{" "}
              {a.size ?? "unknown"} bytes · epoch {a.epoch ?? "?"} · {a.state}
              {a.id && a.state === "closed" && (
                <button disabled={pending} onClick={() => download(a)}>
                  Download{" "}
                  {a.mediaType === "application/x-graphlab-terminal"
                    ? "recording"
                    : "PCAPNG"}{" "}
                  ({a.size} bytes)
                </button>
              )}
            </li>
          ))}
      </ul>
      <small>
        {runs.length} retained runs · closed segments only · SHA-256 verified on
        download
      </small>
      {inspected && (
        <>
          <Telemetry
            key={inspected!.id}
            run={inspected!}
            csrf={csrf}
            api={api}
            selectedEdge={selectedEdge}
            onEdge={onEdge}
            onRates={onRates}
          />
          <ApplicationTelemetry
            key={`application-${inspected!.id}`}
            runId={inspected!.id}
            node={selectedNode}
            csrf={csrf}
            api={api}
          />
          <RecordedTerminal
            key={inspected!.id}
            run={inspected!}
            csrf={csrf}
            api={api}
            selectedNode={selectedNode}
          />
        </>
      )}
    </section>
  );
}
