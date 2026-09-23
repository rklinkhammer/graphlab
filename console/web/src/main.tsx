import React, { useEffect, useState, useCallback } from "react";
import { createRoot } from "react-dom/client";
import {
  ReactFlow,
  Background,
  Controls,
  Handle,
  Position,
  BaseEdge,
  EdgeLabelRenderer,
  applyNodeChanges,
  type NodeProps,
  type EdgeProps,
  type Node,
  type Edge,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import "./style.css";
import { Execution } from "./execution";
import { graph, type Inventory } from "./graph";
function LogicalNode({ data }: NodeProps) {
  return (
    <div
      className={`logical-node ${data.kind === "ovs-switch" ? "switch" : ""}`}
    >
      <Handle type="target" position={Position.Left} />
      <small>{String(data.kind)}</small>
      <strong>{String(data.label)}</strong>
      <span>○ {String(data.state)}</span>
      {data.domain ? <em>Shared OVS</em> : null}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
function Cable({
  id,
  sourceX: sx,
  sourceY: sy,
  targetX: tx,
  targetY: ty,
  data,
  style,
  markerEnd,
}: EdgeProps) {
  const offset =
    Number(data?.offset ?? 0) -
    (Math.abs(sy - ty) < 1 && Math.abs(sx - tx) > 260 ? 240 : 0);
  const self = Math.abs(sy - ty) < 1 && sx > tx && sx - tx < 240;
  const mx = (sx + tx) / 2,
    my = (sy + ty) / 2 + offset;
  const path = self
    ? `M ${sx} ${sy} C ${sx + 100} ${sy - 140 - offset}, ${tx - 100} ${ty - 140 - offset}, ${tx} ${ty}`
    : `M ${sx} ${sy} Q ${mx} ${my} ${tx} ${ty}`;
  return (
    <>
      <BaseEdge id={id} path={path} style={style} markerEnd={markerEnd} />
      <EdgeLabelRenderer>
        <span
          className="edge-label nodrag nopan"
          style={{
            transform: `translate(-50%,-50%) translate(${mx}px,${self ? sy - 105 - offset : (sy + ty) / 2 + offset / 2 - 40}px)`,
          }}
        >
          {String(data?.label)}
          {data?.rate ? (
            <strong className="edge-rate">{String(data.rate)}</strong>
          ) : null}
        </span>
      </EdgeLabelRenderer>
    </>
  );
}
const nodeTypes = { logical: LogicalNode },
  edgeTypes = { cable: Cable };
async function api(path: string, init?: RequestInit) {
  const response = await fetch(`/api/v1/${path}`, {
    credentials: "same-origin",
    ...init,
  });
  if (!response.ok) {
    const detail = await response.json().catch(() => null);
    throw new Error(
      response.status === 401
        ? "Sign in to continue."
        : response.status === 503
          ? "Agent unavailable. Runtime state is unknown."
          : (detail?.error?.code?.replaceAll("_", " ") ??
            `Request failed (${response.status}).`),
    );
  }
  return response.json();
}
function App() {
  const [selectedNode, setSelectedNode] = useState(""),
    [selectedEdge, setSelectedEdge] = useState("");
  const [rates, setRates] = useState<Record<string, string>>({});
  const updateRates = useCallback(
    (value: Record<string, string>) => setRates(value),
    [],
  );
  const chooseNode = (id: string) => {
    setSelectedNode(id);
    setSelectedEdge("");
  };
  const chooseEdge = (id: string) => {
    setSelectedEdge(id);
    setSelectedNode("");
    setSelection(inventory?.edges.find((e) => e.id === id) ?? null);
  };
  const [execution, setExecution] = useState(false);
  const [csrf, setCsrf] = useState(""),
    [password, setPassword] = useState(""),
    [error, setError] = useState("");
  const [catalog, setCatalog] = useState<
      { hash: string; id: string; nodes: number; edges: number }[]
    >([]),
    [hash, setHash] = useState("");
  useEffect(() => {
    setSelectedNode("");
    setSelectedEdge("");
    setRates({});
  }, [hash]);
  const [inventory, setInventory] = useState<Inventory | null>(null),
    [diagnostics, setDiagnostics] = useState<unknown>(null);
  const [management, setManagement] = useState(false),
    [selection, setSelection] = useState<unknown>(null),
    [refresh, setRefresh] = useState(0);
  const [nodes, setNodes] = useState<Node[]>([]),
    [edges, setEdges] = useState<Edge[]>([]);
  useEffect(() => {
    api("session")
      .then((s) => setCsrf(s.csrf))
      .catch(() => {});
  }, []);
  useEffect(() => {
    if (!csrf) return;
    let cancelled = false;
    Promise.all([api("topologies"), api("diagnostics"), api("capabilities")])
      .then(([c, d, cap]) => {
        if (cancelled) return;
        setCatalog(c.items);
        setDiagnostics(d);
        setExecution(cap.execution);
        setHash((old) => old || c.items[0]?.hash || "");
        setError("");
      })
      .catch((e) => {
        if (!cancelled) {
          setError(e.message);
          setInventory(null);
          if (e.message === "Sign in to continue.") setCsrf("");
        }
      });
    return () => {
      cancelled = true;
    };
  }, [csrf, refresh]);
  useEffect(() => {
    if (!csrf || !hash) return;
    let cancelled = false,
      busy = false;
    setInventory(null);
    setSelection(null);
    async function observe() {
      if (busy) return;
      busy = true;
      try {
        const value = await api(`topologies/${hash}/inventory`);
        if (!cancelled) {
          setInventory(value);
          setError("");
        }
      } catch (e) {
        if (!cancelled) {
          setError((e as Error).message);
          setInventory(null);
          if ((e as Error).message === "Sign in to continue.") setCsrf("");
        }
      } finally {
        busy = false;
      }
    }
    void observe();
    const timer = setInterval(observe, 5000);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [csrf, hash, refresh]);
  useEffect(() => {
    if (selectedNode)
      setSelection(inventory?.nodes.find((n) => n.id === selectedNode) ?? null);
    else if (selectedEdge)
      setSelection(inventory?.edges.find((e) => e.id === selectedEdge) ?? null);
  }, [inventory, selectedNode, selectedEdge]);
  useEffect(() => {
    if (inventory) {
      const view = graph(inventory, management);
      setNodes((old) =>
        view.nodes.map((n) => ({
          ...n,
          position: old.find((o) => o.id === n.id)?.position ?? n.position,
        })),
      );
      setEdges(view.edges);
    } else {
      setNodes([]);
      setEdges([]);
    }
  }, [inventory, management]);
  async function login(e: React.FormEvent) {
    e.preventDefault();
    try {
      const s = await api("login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ password }),
      });
      setPassword("");
      setCsrf(s.csrf);
      setError("");
    } catch (e) {
      setError((e as Error).message);
    }
  }
  async function logout() {
    try {
      await api("logout", {
        method: "POST",
        headers: { "X-CSRF-Token": csrf },
      });
      setCsrf("");
      setInventory(null);
      setCatalog([]);
      setDiagnostics(null);
      setSelection(null);
      setHash("");
    } catch (e) {
      setError((e as Error).message);
    }
  }
  if (!csrf)
    return (
      <main className="login">
        <div className="wordmark">
          ◈ GRAPHLAB <span>LOCAL</span>
        </div>
        <h1>Your lab, in view.</h1>
        <p>Inspect topology, resource mappings and diagnostics.</p>
        <form onSubmit={login}>
          <label htmlFor="password">Operator credential</label>
          <input
            id="password"
            type="password"
            autoComplete="current-password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            required
          />
          <button>Sign in</button>
        </form>
        {error && <p role="alert">{error}</p>}
        <small>Local console · Authenticated access</small>
      </main>
    );
  return (
    <div className="shell">
      <header>
        <div className="wordmark">
          ◈ GRAPHLAB <span>Topology console</span>
        </div>
        <div className="actions">
          <span className="badge">
            {execution
              ? inventory?.capturePolicy?.required
                ? "M3 CAPTURE-FIRST"
                : "M2 DEVELOPMENT"
              : "READ ONLY"}
          </span>
          <button onClick={() => setRefresh((v) => v + 1)}>Refresh</button>
          <button onClick={logout}>Sign out</button>
        </div>
      </header>
      <div className="workspace">
        <aside className="sidebar">
          <small>WORKSPACE</small>
          <h1>Topologies</h1>
          <p>Immutable, validated revisions</p>
          <nav aria-label="Topologies">
            {catalog.map((t) => (
              <button
                key={t.hash}
                aria-pressed={hash === t.hash}
                onClick={() => setHash(t.hash)}
              >
                <strong>{t.id}</strong>
                <span>
                  {t.nodes} nodes · {t.edges} edges
                </span>
              </button>
            ))}
          </nav>
          <div className="note">
            <strong>One shared OVS daemon</strong>
            <p>
              Logical switches share a host failure domain. A bridge outage
              differs from a daemon outage.
            </p>
          </div>
        </aside>
        <main className="main-panel">
          <div className="toolbar">
            <div>
              <small>LOGICAL GRAPH</small>
              <h2>{inventory?.topologyId ?? "Select a topology"}</h2>
            </div>
            <label>
              <input
                type="checkbox"
                checked={management}
                onChange={(e) => setManagement(e.target.checked)}
              />{" "}
              Management layer
            </label>
          </div>
          {error && (
            <p className="error" role="alert">
              {error}
            </p>
          )}
          <div className="summary">
            <span>{inventory?.nodes.length ?? "—"} nodes</span>
            <span>{inventory?.edges.length ?? "—"} data edges</span>
            <span>
              ○ Runtime mappings:{" "}
              {inventory?.runtimeFreshness === "snapshot"
                ? "identity snapshots"
                : "unknown"}
            </span>
          </div>
          <div className="canvas" aria-label="Topology graph">
            <ReactFlow
              key={hash}
              nodes={nodes}
              edges={edges.map((e) => ({
                ...e,
                selected: e.id === selectedEdge,
                data: { ...e.data, rate: rates[e.id] },
              }))}
              nodeTypes={nodeTypes}
              edgeTypes={edgeTypes}
              onNodesChange={(changes) =>
                setNodes((n) => applyNodeChanges(changes, n))
              }
              onNodeClick={(_, node) => {
                chooseNode(node.id);
                setSelection(
                  inventory?.nodes.find((n) => n.id === node.id) ??
                    inventory?.management.networks[node.id.slice(11)],
                );
              }}
              onEdgeClick={(_, edge) => {
                chooseEdge(edge.id);
                setSelection(
                  inventory?.edges.find((e) => e.id === edge.id) ??
                    inventory?.managementAttachments.find(
                      (a) => `management/${a.endpoint}` === edge.id,
                    ),
                );
              }}
              nodesConnectable={false}
              edgesReconnectable={false}
              deleteKeyCode={null}
              fitView
              minZoom={0.05}
              maxZoom={2}
            >
              <Background gap={24} color="#d8dfe5" />
              <Controls showInteractive={false} />
            </ReactFlow>
          </div>
          <details className="inventory-table">
            <summary>Accessible inventory · all nodes and data edges</summary>
            <ul>
              {inventory?.nodes.map((n) => (
                <li key={n.id}>
                  <button
                    onClick={() => {
                      setSelection(n);
                      chooseNode(n.id);
                    }}
                  >
                    {n.id} · {n.kind} · runtime {n.runtime.state}
                  </button>
                </li>
              ))}
              {inventory?.edges.map((e) => (
                <li key={e.id}>
                  <button
                    onClick={() => {
                      setSelection(e);
                      chooseEdge(e.id);
                    }}
                  >
                    {e.id}: {e.endpoints.join(" ↔ ")} · runtime{" "}
                    {e.runtime.state}
                  </button>
                </li>
              ))}
            </ul>
          </details>
          {execution && (
            <Execution
              key={hash}
              hash={hash}
              csrf={csrf}
              api={api}
              required={inventory?.capturePolicy?.required ?? true}
              selectedNode={selectedNode}
              selectedEdge={selectedEdge}
              onEdge={chooseEdge}
              onRates={updateRates}
            />
          )}
        </main>
        <aside className="inspector">
          <small>INSPECTOR</small>
          <h2>{selection ? "Resource details" : "Observation status"}</h2>
          {selection ? (
            <>
              <button
                onClick={() => {
                  setSelection(null);
                  setSelectedNode("");
                  setSelectedEdge("");
                }}
              >
                Clear selection
              </button>
              {execution && (
                <button
                  onClick={() =>
                    document
                      .getElementById(
                        selectedNode ? "node-consoles" : "link-performance",
                      )
                      ?.scrollIntoView({ behavior: "smooth" })
                  }
                >
                  {selectedNode ? "Open node console" : "Open link performance"}
                </button>
              )}
              <pre>{JSON.stringify(selection, null, 2)}</pre>
            </>
          ) : (
            <>
              <div className="status">
                ○{" "}
                {inventory?.runtimeFreshness === "snapshot"
                  ? "Runtime identity snapshots"
                  : "Unknown runtime state"}
              </div>
              <p>
                {inventory?.runtimeFreshness === "snapshot"
                  ? "Agent-owned identities are shown as snapshots. Continuous runtime monitoring is not available."
                  : "No executor-owned mappings are available. Unknown does not mean stopped, healthy, or zero traffic."}
              </p>
              <dl>
                <dt>Runtime observed</dt>
                <dd>{inventory?.runtimeObservedAt ?? "Not observed"}</dd>
                <dt>Snapshot generated</dt>
                <dd>{inventory?.generatedAt ?? "—"}</dd>
                <dt>Failure domain</dt>
                <dd>host/shared-ovs</dd>
              </dl>
              <details>
                <summary>Host diagnostics</summary>
                <pre>{JSON.stringify(diagnostics, null, 2)}</pre>
              </details>
            </>
          )}
          <div className="note">
            Canvas positions are temporary view settings. Moving a node does not
            change its topology.
          </div>
        </aside>
      </div>
    </div>
  );
}
createRoot(document.getElementById("root")!).render(<App />);
