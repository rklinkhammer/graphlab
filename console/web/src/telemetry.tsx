import React,{useEffect,useState} from 'react';
type Api=(p:string,i?:RequestInit)=>Promise<any>;
export function Telemetry({run,csrf,api}:{run:{id:string;state:string;revision:string};csrf:string;api:Api}){
 const [data,setData]=useState<any>({items:[],current:{}}),[faults,setFaults]=useState<any>({items:{}}),[events,setEvents]=useState<any[]>([]),[edge,setEdge]=useState(''),[direction,setDirection]=useState('a-to-b'),[delay,setDelay]=useState(100),[loss,setLoss]=useState(0),[duration,setDuration]=useState(10),[preview,setPreview]=useState<any>(null),[error,setError]=useState(''),[job,setJob]=useState<any>(null),[jobId,setJobId]=useState('');
 const [resolution,setResolution]=useState(1);
 const [pollErrors,setPollErrors]=useState<Record<string,string>>({});
 const [observed,setObserved]=useState(0),[clock,setClock]=useState(performance.now());
 const stale=(s:any)=>s.stale||clock-observed>=3000;
 const post=(path:string,body:any)=>api(`runs/${run.id}/${path}`,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify(body)});
 useEffect(()=>{setEdge('');setPreview(null);setJobId('');},[run.id]);
 useEffect(()=>{
  let live=true,busy=false;
  const updateError=(key:string,message:string)=>{if(live)setPollErrors(old=>({...old,[key]:message}));};
  async function refresh(){
   if(busy)return;
   busy=true;
   const tasks=[
    post('telemetry/query',{resolutionSeconds:resolution,windowSeconds:resolution===1?3600:resolution===10?86400:604800,limit:2000,...(edge?{edge}:{})})
     .then(t=>{if(live){setData(t);setObserved(performance.now());}updateError('history','');}).catch(e=>updateError('history',e.message)),
    api(`runs/${run.id}/faults`).then(f=>{if(live)setFaults(f);updateError('faults','');}).catch(e=>updateError('faults',e.message)),
    api(`runs/${run.id}/timeline`).then(e=>{if(live)setEvents(e.items);updateError('timeline','');}).catch(e=>updateError('timeline',e.message))
   ];
   await Promise.allSettled(tasks);busy=false;
  }
  void refresh();
  const timer=setInterval(()=>{setClock(performance.now());void refresh();},1000);
  return()=>{live=false;clearInterval(timer);};
 },[run.id,api,resolution,edge]);

 useEffect(()=>{if(!jobId)return;let live=true;const refresh=()=>void api(`jobs/${jobId}`).then(j=>{if(live)setJob(j);}).catch(e=>setError(e.message));refresh();const timer=setInterval(refresh,500);return()=>{live=false;clearInterval(timer);};},[jobId,api]);
 const spec={edge,direction,kind:'netem',delayMs:delay,lossPercent:loss,durationSeconds:duration};
 const signature=JSON.stringify(spec);
 useEffect(()=>setPreview(null),[signature]);
 async function action(remove?:string){try{const result=await post(remove?'faults/remove':'faults',{...(remove?{faultId:remove}:{fault:spec}),expectedRevision:faults.revision??run.revision,idempotencyKey:crypto.randomUUID()});setJobId(result.jobId);setPreview(null);}catch(e){setError((e as Error).message);}}
 const rows=Object.values({...Object.fromEntries((data.items??[]).map((s:any)=>[s.edge,{...s,stale:true}])),...(data.current??{})}) as any[];const selected=rows.find(s=>s.edge===edge);const history=(data.items??[]).filter((s:any)=>s.edge===edge);const max=Math.max(1,...history.flatMap((s:any)=>[s.forwardBitsPerSecond??0,s.reverseBitsPerSecond??0]));
 function paths(key:string){let segments:string[]=[],line='',previous=0;const start=Number(history[0]?.bucketUnixSeconds??0),end=Number(history.at(-1)?.bucketUnixSeconds??start);history.forEach((s:any)=>{const time=Number(s.bucketUnixSeconds);if(previous&&time-previous>resolution*1.5){if(line)segments.push(line);line='';}previous=time;if(s[key]==null||s.gapCount>0){if(line)segments.push(line);line='';return;}line+=`${line?'L':'M'}${(time-start)*560/Math.max(1,end-start)},${100-s[key]/max*95} `;});if(line)segments.push(line);return segments;}
 return <section aria-label="Telemetry and faults"><h3>Directional telemetry and faults</h3><p>One counter source per edge. Software interface rates are not physical-wire utilization. A→B follows the topology's ordered endpoints. Gaps and first samples have no rate.</p>
 <label>History <select aria-label="Telemetry history resolution" value={resolution} onChange={e=>setResolution(Number(e.target.value))}><option value={1}>1 second · last hour</option><option value={10}>10 seconds · last day</option><option value={60}>1 minute · last week</option></select></label>
 <table><thead><tr><th>Edge</th><th>A→B bit/s</th><th>B→A bit/s</th><th>Observation</th></tr></thead><tbody>{rows.map(s=><tr key={s.edge}><td><button onClick={()=>setEdge(s.edge)}>{s.edge}</button></td><td>{stale(s)?'—':s.forwardBitsPerSecond?.toFixed(0)??'—'}</td><td>{stale(s)?'—':s.reverseBitsPerSecond?.toFixed(0)??'—'}</td><td>{stale(s)?'stale':s.gapReason??'current'} · admin {String(s.adminUp??'unknown')} · carrier {String(s.carrierUp??'unknown')}</td></tr>)}</tbody></table>
 {edge&&<><h4>{edge} · {selected?.endpoints?.join(' → ')}</h4><svg viewBox="0 0 560 110" role="img" aria-label={`${edge} directional rate history; solid forward, dashed reverse`} style={{maxWidth:700,width:'100%',background:'#eef5f5'}}>{paths('forwardBitsPerSecond').map((d,i)=><path key={`a${i}`} d={d} stroke="#087f8c" fill="none"/>)}{paths('reverseBitsPerSecond').map((d,i)=><path key={`b${i}`} d={d} stroke="#923f17" strokeDasharray="5 3" fill="none"/>)}</svg><p>Solid: A→B; dashed: B→A. Peak scale {max.toFixed(0)} bit/s. {data.truncated?'History truncated by point limit.':''} Oldest retained bucket: {data.oldestAvailableUnixSeconds?new Date(Number(data.oldestAvailableUnixSeconds)*1000).toISOString():'none'}. Retention is also limited by a shared 100,000-row capacity.</p><details><summary>Counter source, mapping, qdisc and epoch</summary><pre>{JSON.stringify(selected,null,2)}</pre></details></>}
 <form onSubmit={e=>{e.preventDefault();void post('faults/preview',{fault:spec}).then(setPreview).catch(e=>setError(e.message));}}>
 <label>Fault edge <select aria-label="Fault edge" value={edge} onChange={e=>setEdge(e.target.value)}><option value="">Select edge</option>{rows.map(s=><option key={s.edge}>{s.edge}</option>)}</select></label>
 <label>Direction <select aria-label="Fault direction" value={direction} onChange={e=>setDirection(e.target.value)}><option value="a-to-b">A→B</option><option value="b-to-a">B→A</option></select></label>
 <label>Delay ms <input type="number" min="0" max="5000" value={delay} onChange={e=>setDelay(Number(e.target.value))}/></label><label>Loss percent <input type="number" min="0" max="100" value={loss} onChange={e=>setLoss(Number(e.target.value))}/></label><label>Duration seconds <input type="number" min="1" max="3600" value={duration} onChange={e=>setDuration(Number(e.target.value))}/></label>
 <button disabled={!edge||!['ready','stopped'].includes(run.state)}>Preview fault placement</button></form>
 {preview&&<div><pre>{JSON.stringify(preview,null,2)}</pre><button onClick={()=>void action()}>Apply directional fault</button></div>}
 {job&&<p role="status">Fault job: {job.state}{job.error?` · ${job.error}`:''}{['queued','running'].includes(job.state)&&job.operation==='fault.apply'&&<button onClick={()=>void api(`jobs/${jobId}/cancel`,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:'{}'}).catch(e=>setError(e.message))}>Cancel fault job</button>}</p>}
 <ul>{Object.values(faults.items??{}).map((f:any)=><li key={f.id}>{f.edge} {f.direction} · {f.delayMs} ms / {f.lossPercent}% · {f.state} · {f.durationSeconds}s {f.state==='active'&&<button onClick={()=>void action(f.id)}>Remove fault</button>}</li>)}</ul>
 {data.collectorError&&<p role="alert">Collector: {data.collectorError}</p>}{Object.entries(pollErrors).filter(([,v])=>v).map(([k,v])=><p role="alert" key={k}>{k}: {v}</p>)}{error&&<p role="alert">{error}</p>}<details><summary>Correlated timeline ({events.length})</summary><ol>{events.slice(-100).reverse().map((e,i)=><li key={i}>{e.at} · {e.kind}<pre>{JSON.stringify(e.detail,null,2)}</pre></li>)}</ol></details>
 </section>;
}
