import React, {useEffect, useRef, useState} from "react";
import {ApiError} from "./api-error";
type Api = (path: string, init?: RequestInit) => Promise<any>;
// Parent key includes selection; the effect fence also rejects responses from old credentials.
export function SourceControls({runId,node,csrf,api}: {runId:string,node:string,csrf:string,api:Api}) {
  const [data,setData] = useState<any>(null);
  const [queryError,setQueryError] = useState("");
  const [commandError,setCommandError] = useState("");
  const [pending,setPending] = useState(false);
  const [outcome,setOutcome] = useState<any>(null);
  const retry = useRef<any>(null);
  const fence = useRef(0);
  const observation = useRef(0);
  useEffect(() => {
    let live = true, busy = false;
    async function refresh() {
      if (busy || !node) return;
      busy = true;
      const version = observation.current;
      try {
        const next = await api(`runs/${runId}/source-controls/query`, {
          method:"POST", headers:{"Content-Type":"application/json","X-CSRF-Token":csrf},
          body:JSON.stringify({node}),
        });
        if (!live || version !== observation.current) return;
        setData(next);
        setQueryError("");
        if (retry.current) {
          const c = next.commands?.find((c:any) => c.request.requestId === retry.current.requestId);
          if (c) {
            setOutcome(c);
            if (c.outcome !== "requested") {
              retry.current = null;
              setPending(false);
              setCommandError("");
            }
          }
        }
      } catch(e) {
        if (live && version === observation.current) {
          setQueryError((e as Error).message);
          setData(null);
        }
      } finally { busy = false; }
    }
    void refresh();
    const timer = setInterval(refresh,1500);
    return () => { live = false; fence.current++; clearInterval(timer); };
  },[runId,node,csrf,api]);
  async function command(source:string, action:string) {
    const body = retry.current ?? {
      apiVersion:"graphlab.source-control/v1",node,instance:data.instance,
      epoch:data.capability.epoch,source,action,requestId:crypto.randomUUID(),
    };
    const currentFence = fence.current;
    retry.current = body;
    observation.current++;
    setPending(true);
    setCommandError("");
    setOutcome({outcome:"requested",request:body});
    try {
      const c = await api(`runs/${runId}/source-controls/command`, {
        method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrf},
        body:JSON.stringify(body),
      });
      if (currentFence !== fence.current || retry.current !== body) return;
      setOutcome(c);
      if (c.outcome !== "requested") { retry.current = null; setPending(false); }
    } catch(e) {
      if (currentFence !== fence.current || retry.current !== body) return;
      setPending(false);
      const message = (e as Error).message;
      setCommandError(message);
      // These statuses are definitive admission rejections, not evidence of an accepted command.
      // Timeouts, transport failures and server errors remain uncertain and keep the same identity.
      if (e instanceof ApiError && [400,401,403,404,405,409,422,429].includes(e.status)) {
        retry.current = null;
        observation.current++;
        setData(null); // Require a query started after rejection before enabling a fresh command.
        setOutcome({request:body,outcome:"failed",error:message});
      } else {
        setOutcome({request:body,outcome:"unknown — awaiting reconciliation"});
      }
    }
  }
  return <section aria-label="Source controls"><h3>Source controls</h3>
    {!node ? <p>Select a workload node to inspect advertised sources.</p> : <p>Target: {node} · {data?.instance ?? "identity unavailable"}</p>}
    <p>Pause stops new source generation. Receiving continues. Source resume cannot release a quiesced run; already sent traffic is not withdrawn.</p>
    {queryError && <p role="alert">Source status unavailable: {queryError}</p>}
    {commandError && <p role="alert">Source command: {commandError}</p>}
    {data?.error && <p>{data.error}</p>}
    {data?.capability?.sources?.map((s:any) => <article key={s.id}><h4>{s.id}</h4><p>{s.state} · {s.generatedDatagrams} generated datagrams · run {data.runState}</p>
      <button disabled={pending || Boolean(retry.current)} onClick={() => void command(s.id,"pause")}>Pause source {s.id}</button>{" "}
      <button disabled={pending || Boolean(retry.current)} onClick={() => void command(s.id,"resume")}>Resume source {s.id}</button></article>)}
    {outcome && <p role="status">{outcome.request.source}: {outcome.request.action} — {outcome.outcome}{outcome.error ? ` (${outcome.error})` : ""}</p>}
    {commandError && retry.current && !pending && <button onClick={() => void command(retry.current.source,retry.current.action)}>Retry same request</button>}
    {data?.commands?.length > 0 && <details><summary>Retained source command outcomes</summary><pre>{JSON.stringify(data.commands,null,2)}</pre></details>}
  </section>;
}
