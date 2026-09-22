import React,{useEffect,useState} from 'react';
type Run={id:string;state:string;revision:string;topologyHash:string;resources?:{key:string;state:string}[];captureCoverage?:string;observation?:unknown};
type Job={id:string;operation:string;state:string;error:string|null;cancelRequested:boolean};
type Api=(path:string,init?:RequestInit)=>Promise<any>;
export function Execution({hash,csrf,api}:{hash:string;csrf:string;api:Api}){
 const [runs,setRuns]=useState<Run[]>([]),[run,setRun]=useState<Run|null>(null),[job,setJob]=useState<Job|null>(null),[jobId,setJobId]=useState(''),[error,setError]=useState(''),[ack,setAck]=useState(false),[pending,setPending]=useState(false);
 useEffect(()=>{let cancelled=false;async function refresh(){try{const list=await api('runs');if(cancelled)return;setRuns(list.items);const active=list.items.find((r:Run)=>r.state!=='destroyed');if(active){const detail=await api(`runs/${active.id}`);if(!cancelled)setRun(detail);}else setRun(null);if(jobId){const value=await api(`jobs/${jobId}`);if(!cancelled)setJob(value);}}catch(e){if(!cancelled)setError((e as Error).message);}}
 void refresh();const timer=setInterval(refresh,1200);return()=>{cancelled=true;clearInterval(timer);};},[jobId,api]);
 async function submit(operation:string){setPending(true);setError('');try{
 const key=crypto.randomUUID();let path='runs',body:Record<string,unknown>={topologyHash:hash,idempotencyKey:key,developmentMode:ack};
 if(operation!=='start'){if(!run)return;path=`runs/${run.id}/operations`;body={operation,expectedRevision:run.revision,idempotencyKey:key};}
 const value=await api(path,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify(body)});setJobId(value.jobId);
 }catch(e){setError((e as Error).message);}finally{setPending(false);}}
 async function cancel(){if(!jobId)return;try{await api(`jobs/${jobId}/cancel`,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:'{}'});}catch(e){setError((e as Error).message);}}
 const busy=pending||job?.state==='queued'||job?.state==='running';
 return <section className="execution" aria-label="Run controls"><div><strong>M2 development execution</strong><p>Capture coverage is unavailable. Required-capture and QEMU topologies are rejected.</p></div>
 {!run?<><label><input type="checkbox" checked={ack} onChange={e=>setAck(e.target.checked)}/> I accept running without capture coverage.</label><button disabled={!ack||busy||!hash} onClick={()=>submit('start')}>Start selected topology</button></>:<><p>Run {run.id} · <strong>{run.state}</strong> · revision {run.revision}</p><div className="actions"><button disabled={busy||run.state!=='ready'} onClick={()=>submit('stop')}>Quiesce</button><button disabled={busy||run.state!=='stopped'} onClick={()=>submit('resume')}>Resume</button><button disabled={busy} onClick={()=>submit(run.state==='reconciling'?'recover':'destroy')}>{run.state==='reconciling'?'Recover and clean up':'Destroy run'}</button></div><details><summary>Runtime resource identities</summary><pre>{JSON.stringify(run,null,2)}</pre></details></>}
 {job&&<p role="status">Job {job.operation}: {job.state}{job.error?` · ${job.error}`:''}{busy&&job.operation==='start'&&<button onClick={cancel}>Cancel start</button>}</p>}{error&&<p role="alert">{error}</p>}<small>{runs.length} retained run records · agent-owned journal</small></section>;
}
