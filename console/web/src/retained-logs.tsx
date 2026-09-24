import React,{useEffect,useState} from "react";
type Api=(path:string,init?:RequestInit)=>Promise<any>;
export function RetainedLogs({runId,node,csrf,api}:{runId:string,node:string,csrf:string,api:Api}) {
 const [page,setPage]=useState<any>(null),[source,setSource]=useState(""),[cursor,setCursor]=useState<string|null>(null),[selected,setSelected]=useState<any>(null),[queryError,setQueryError]=useState(""),[actionError,setActionError]=useState(""),[search,setSearch]=useState("");
 useEffect(()=>{
   let live=true,busy=false;setPage(null);setSelected(null);setQueryError("");
   async function refresh(){if(busy)return;busy=true;try{
     const result=await api(`runs/${runId}/process-logs/query`,{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrf},body:JSON.stringify({node,...(source?{source}:{}),...(cursor?{cursor}:{})})});
     if(live){setPage(result);setQueryError("");}
   }catch(e){if(live)setQueryError((e as Error).message);}finally{busy=false;}}
   void refresh();const timer=cursor?null:setInterval(refresh,3000);return()=>{live=false;if(timer)clearInterval(timer);};
 },[runId,node,source,cursor,csrf,api]);
 // Scope key is supplied by the parent; source/page transitions invalidate in-flight downloads.
 const selection=React.useRef(0);
 useEffect(()=>{selection.current++;setActionError("");return()=>{selection.current++;};},[runId,node,source,cursor]);
 async function load(id:string,download=false){const generation=++selection.current;setActionError("");try{
   const item=await api(`runs/${runId}/process-logs/download`,{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrf},body:JSON.stringify({node,id})});
   if(generation!==selection.current)return;
   setSelected(item);setActionError("");
   if(download){const bytes=Uint8Array.from(atob(item.base64),(c)=>c.charCodeAt(0));const url=URL.createObjectURL(new Blob([bytes],{type:"application/octet-stream"}));const a=document.createElement("a");a.href=url;a.download=`${node}-${id}-process.log`;a.click();URL.revokeObjectURL(url);}
 }catch(e){if(generation===selection.current){setActionError((e as Error).message);setSelected(null);}}}
 const text=selected?new TextDecoder().decode(Uint8Array.from(atob(selected.base64),(c)=>c.charCodeAt(0))):"";
 return <section aria-label="Retained process logs"><h4>Durable process-log snapshots</h4>
 <p>Immutable bounded runtime tails, retained after destruction. Snapshots may overlap; output between polls or before attachment may be omitted. Not a continuous log or serial recording.</p>
 <label>Log source <select value={source} onChange={e=>{setSource(e.target.value);setCursor(null);}}><option value="">All retained sources</option>{page?.sources?.map((s:any)=><option key={s.id} value={s.id}>{s.id}</option>)}</select></label>
 {queryError&&<p role="alert">Catalog: {queryError}</p>}{actionError&&<p role="alert">Artifact: {actionError}</p>}{page?.error&&<p role="alert">Collection diagnostic: {page.error}</p>}
 {page?.sourceCapacityReached&&<p role="alert">Source catalog capacity reached; additional sources may be omitted.</p>}
 {page?.sources?.map((s:any)=><p key={s.id}>{s.id}: {s.stale?"stale / retained":"recent collection"} · {s.evicted} snapshots evicted · {s.error||"no current source error"}</p>)}
 <p>{cursor?"Frozen older page":"Newest snapshots (refreshing)"}</p>
 <button disabled={!page?.nextCursor} onClick={()=>setCursor(page.nextCursor)}>Older log snapshots</button>{" "}<button onClick={()=>{setCursor(null);setSelected(null);setActionError("");selection.current++;}}>Return to newest logs</button>
 {page&&!page.items?.length&&<p>No retained snapshots in this scope. Uncollected output is unavailable.</p>}
 <ul>{page?.items?.map((item:any)=><li key={item.id}><button onClick={()=>void load(item.id)}>Inspect log {item.id}</button>{" "}{item.sourceId} · {item.observedAt} · {item.size} bytes · {item.gap} {item.truncated?"· byte truncated":""}{" "}<button onClick={()=>void load(item.id,true)}>Download log {item.id}</button></li>)}</ul>
 {selected&&<><p>Retained artifact {selected.id} · generation {selected.generation} · SHA-256 {selected.sha256}. Checksum verified by the server. Display replaces invalid UTF-8; download preserves collected bytes.</p><label>Search selected snapshot <input value={search} onChange={e=>setSearch(e.target.value)}/></label><pre className="console-log">{search?text.split("\n").filter(line=>line.includes(search)).join("\n"):text||"Empty collected output."}</pre></>}
 </section>;
}
