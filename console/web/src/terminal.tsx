import React, { useEffect, useRef, useState } from "react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import { NodeLogs } from "./node-logs";
type Api = (path: string, init?: RequestInit) => Promise<any>;
type Session = {
  id: string;
  node: string;
  kind: string;
  recordInput?: boolean;
};
type Run = {
  id: string;
  state: string;
  resources?: {
    kind: string;
    logical: string;
    state?: string;
    configuration?: any;
  }[];
};
export function RecordedTerminal({
  run,
  csrf,
  api,
  selectedNode = "",
}: {
  run: Run;
  csrf: string;
  api: Api;
  selectedNode?: string;
}) {
  const [sessions, setSessions] = useState<Session[]>([]),
    [selected, setSelected] = useState(""),
    [node, setNode] = useState(selectedNode),
    [inputRecording, setInputRecording] = useState(false),
    [error, setError] = useState(""),
    [writer, setWriter] = useState(false),
    [reconnect, setReconnect] = useState(0),
    [connection, setConnection] = useState("Select a session"),
    [view, setView] = useState("terminal"),
    [notice, setNotice] = useState(""),
    [opening, setOpening] = useState(false);
  const host = useRef<HTMLDivElement>(null),
    token = useRef(""),
    generation = useRef(0),
    active = useRef("");
  const currentNode = useRef(node),
    mounted = useRef(true);
  currentNode.current = node;
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);
  const session = sessions.find((s) => s.id === selected),
    kind = session?.kind;
  async function command(
    operation: string,
    params: Record<string, unknown> = {},
    id = selected,
  ) {
    return api(`runs/${run.id}/terminal`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
      body: JSON.stringify({
        operation,
        ...(id ? { sessionId: id } : {}),
        params,
      }),
    });
  }
  useEffect(() => {
    if (!selectedNode) return;
    setNode(selectedNode);
    setSelected("");
  }, [selectedNode]);
  useEffect(() => {
    let live = true,
      busy = false;
    async function refresh() {
      if (busy) return;
      busy = true;
      try {
        const r = await command("list");
        if (live) setSessions(r.items);
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
  }, [run.id, csrf]);
  useEffect(() => {
    if (!selected || !host.current) return;
    let disposed = false,
      ws: WebSocket | null = null,
      sequence = "0",
      pending = false,
      pendingSince = 0;
    active.current = selected;
    const epoch = ++generation.current;
    token.current = "";
    setWriter(false);
    setError("");
    setNotice("");
    setConnection("Connecting");
    const term = new Terminal({
      convertEol: false,
      scrollback: 3000,
      allowProposedApi: false,
      disableStdin: false,
      allowTransparency: false,
    });
    const fit = new FitAddon();
    term.loadAddon(fit);
    term.open(host.current);
    fit.fit();
    const handlers = [0, 1, 2, 8, 52].map((code) =>
      term.parser.registerOscHandler(code, () => true),
    );
    const input = term.onData((data) => {
      if (disposed || !token.current) return;
      const bytes = new TextEncoder().encode(data);
      if (bytes.length > 1024) {
        setError("Input is limited to 1024 bytes per message.");
        return;
      }
      const base64 = btoa(String.fromCharCode(...bytes));
      void command("input", { token: token.current, base64 }, selected).catch(
        (e) => {
          if (!disposed) {
            token.current = "";
            setWriter(false);
            setError(e.message);
          }
        },
      );
    });
    const resize = () => {
      if (disposed || !host.current?.clientWidth) return;
      fit.fit();
      if (token.current && kind !== "serial")
        void command(
          "resize",
          { token: token.current, rows: term.rows, columns: term.cols },
          selected,
        ).catch((e) => {
          if (!disposed) setError(e.message);
        });
    };
    const observer = new ResizeObserver(resize);
    observer.observe(host.current);
    ws = new WebSocket(
      `${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/api/v1/terminal`,
      "graphlab.terminal.v1",
    );
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      if (!disposed) setConnection("Connected · replaying recorded output");
    };
    ws.onclose = () => {
      if (!disposed) {
        token.current = "";
        setWriter(false);
        setConnection("Disconnected");
        setError("Console disconnected. Reconnect to replay retained output.");
      }
    };
    ws.onerror = () => {
      if (!disposed) setError("Console connection failed.");
    };
    ws.onmessage = (event) => {
      if (disposed) return;
      try {
        if (event.data instanceof ArrayBuffer) {
          const frame = new DataView(event.data);
          if (frame.byteLength < 24) throw new Error("Invalid terminal frame");
          const type = frame.getUint32(16),
            length = frame.getUint32(20);
          if (length !== frame.byteLength - 24)
            throw new Error("Invalid terminal frame length");
          if (type === 1) term.write(new Uint8Array(event.data, 24));
          if (type === 5) {
            setNotice("Recording reports an output gap.");
            term.writeln("\r\n[Output gap]\r\n");
          }
        } else {
          const message = JSON.parse(event.data);
          pending = false;
          if (message.status >= 400)
            throw new Error(
              message.result?.error?.code ??
                message.error ??
                "Console request failed",
            );
          if (message.result.next) sequence = message.result.next;
          if (message.result.partialTail)
            setNotice(
              "Incomplete recording tail; only complete records are shown.",
            );
        }
      } catch (e) {
        setError((e as Error).message);
      }
    };
    const poll = setInterval(() => {
      if (pending && performance.now() - pendingSince > 10000) {
        ws?.close();
        return;
      }
      if (ws?.readyState === WebSocket.OPEN && !pending) {
        pending = true;
        pendingSince = performance.now();
        ws.send(
          JSON.stringify({
            runId: run.id,
            sessionId: selected,
            operation: "replay",
            params: { sequence },
            csrf,
          }),
        );
      }
    }, 250);
    const renew = setInterval(() => {
      if (token.current)
        void command("renew-writer", { token: token.current }, selected).catch(
          (e) => {
            if (!disposed) {
              token.current = "";
              setWriter(false);
              setError(e.message);
            }
          },
        );
    }, 5000);
    return () => {
      disposed = true;
      const oldToken = token.current;
      token.current = "";
      if (generation.current === epoch) ++generation.current;
      if (oldToken)
        void command("release-writer", { token: oldToken }, selected).catch(
          () => {},
        );
      clearInterval(poll);
      clearInterval(renew);
      ws?.close();
      observer.disconnect();
      input.dispose();
      handlers.forEach((h) => h.dispose());
      term.dispose();
    };
  }, [selected, run.id, csrf, reconnect, kind]);
  async function acquire(takeover: boolean) {
    const id = selected,
      epoch = generation.current;
    try {
      const r = await command("acquire", { takeover }, id);
      if (epoch !== generation.current || active.current !== id) {
        void command("release-writer", { token: r.token }, id).catch(() => {});
        return;
      }
      token.current = r.token;
      setWriter(true);
      setError("");
    } catch (e) {
      if (epoch === generation.current) setError((e as Error).message);
    }
  }
  async function release() {
    const old = token.current;
    token.current = "";
    setWriter(false);
    try {
      await command("release-writer", { token: old });
    } catch (e) {
      setError((e as Error).message);
    }
  }
  async function open() {
    if (opening) return;
    setOpening(true);
    const target = node;
    try {
      const r = await api(`runs/${run.id}/terminal`, {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
        body: JSON.stringify({
          operation: "open",
          node: target,
          params: { recordInput: inputRecording },
        }),
      });
      if (!mounted.current) return;
      setSessions((current) => [
        ...current,
        {
          id: r.id,
          node: target,
          kind:
            run.resources?.find((resource) => resource.logical === target)
              ?.kind === "qemu"
              ? "ssh"
              : "docker",
          recordInput: r.recordInput,
        },
      ]);
      if (currentNode.current === target) {
        setSelected(r.id);
        setView("terminal");
      }
    } catch (e) {
      if (mounted.current) setError((e as Error).message);
    } finally {
      if (mounted.current) setOpening(false);
    }
  }
  const resource = run.resources?.find(
    (r) => r.logical === node && (r.kind === "qemu" || r.kind === "container"),
  );
  const serial = sessions.find((s) => s.node === node && s.kind === "serial");
  return (
    <section id="node-consoles" aria-label="Recorded consoles">
      <h3>Node consoles and logs</h3>
      <p>
        Output is recorded and may contain echoed secrets. Writer access expires
        after 15 seconds without renewal. Serial coverage begins at attachment.
      </p>
      <label>
        Workload{" "}
        <select
          aria-label="Workload"
          value={node}
          onChange={(e) => {
            setNode(e.target.value);
            setSelected("");
          }}
        >
          <option value="">Select workload</option>
          {run.resources
            ?.filter((r) => r.kind === "container" || r.kind === "qemu")
            .map((r) => (
              <option key={r.logical}>{r.logical}</option>
            ))}
        </select>
      </label>
      {node && !resource && (
        <p>
          Interactive consoles are unavailable for this resource. Inspect its
          topology and runtime diagnostics.
        </p>
      )}
      <div className="console-tabs">
        <button
          aria-pressed={view === "logs"}
          disabled={!resource}
          onClick={() => setView("logs")}
        >
          Logs
        </button>
        <button
          aria-pressed={view === "terminal"}
          onClick={() => setView("terminal")}
        >
          Terminals
        </button>
        <button
          disabled={!serial}
          onClick={() => {
            setSelected(serial!.id);
            setView("terminal");
          }}
        >
          Serial console
        </button>
        <button
          disabled={
            opening || !resource || !["ready", "stopped"].includes(run.state)
          }
          onClick={open}
        >
          {resource?.kind === "qemu" ? "SSH console" : "Container shell"}
        </button>
      </div>
      {resource?.kind === "qemu" && !serial && (
        <p>
          Serial session not registered yet. Guest networking is not required
          once attached.
        </p>
      )}
      <label>
        <input
          type="checkbox"
          checked={inputRecording}
          onChange={(e) => setInputRecording(e.target.checked)}
        />{" "}
        Record exact input for the new shell
      </label>
      <button
        disabled={
          opening || !resource || !["ready", "stopped"].includes(run.state)
        }
        onClick={open}
      >
        Open recorded session
      </button>
      {view === "logs" && resource && (
        <NodeLogs
          key={`${run.id}/${node}`}
          runId={run.id}
          node={node}
          csrf={csrf}
          api={api}
        />
      )}
      <div hidden={view !== "terminal"}>
        <div className="console-tabs" aria-label="Console sessions">
          {sessions
            .filter((s) => !node || s.node === node)
            .map((s) => (
              <button
                key={s.id}
                aria-pressed={selected === s.id}
                onClick={() => setSelected(s.id)}
              >
                {s.node} · {s.kind} · {s.id.slice(0, 8)}
              </button>
            ))}
        </div>
        <label>
          Console{" "}
          <select
            aria-label="Console"
            value={selected}
            onChange={(e) => setSelected(e.target.value)}
          >
            <option value="">Select session</option>
            {sessions.map((s) => (
              <option key={s.id} value={s.id}>
                {s.node} · {s.kind} · {s.id}
              </option>
            ))}
          </select>
        </label>
        {selected && (
          <>
            <p role="status">
              {connection} ·{" "}
              {writer ? "Writer lease active" : "Read-only viewer"} · output
              recording enabled{" "}
              {session?.recordInput ? "· EXACT INPUT RECORDING ENABLED" : ""}
            </p>
            <button
              disabled={run.state === "destroyed"}
              onClick={() => acquire(false)}
            >
              Acquire writer
            </button>
            <button
              disabled={run.state === "destroyed"}
              onClick={() => acquire(true)}
            >
              Take over writer
            </button>
            <button disabled={!writer} onClick={release}>
              Release keyboard
            </button>
            <button onClick={() => setReconnect((x) => x + 1)}>
              Reconnect and replay
            </button>
            <button
              disabled={run.state === "destroyed" || kind === "serial"}
              onClick={() =>
                void command("close")
                  .then(() => {
                    token.current = "";
                    setWriter(false);
                    setConnection("Closed · retained replay available");
                  })
                  .catch((e) => setError(e.message))
              }
            >
              Close shell
            </button>
          </>
        )}
        <div ref={host} className="terminal-host" />
      </div>
      {notice && <p role="note">{notice}</p>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
