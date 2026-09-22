import React,{useEffect,useRef,useState} from 'react';
import {Terminal} from '@xterm/xterm';
import {FitAddon} from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';
type Api=(path:string,init?:RequestInit)=>Promise<any>;
type Session={id:string;node:string;kind:string;recordInput?:boolean};
export function RecordedTerminal({run,csrf,api}:{run:{id:string;state:string;resources?:{kind:string;logical:string}[]};csrf:string;api:Api}){
 const [sessions,setSessions]=useState<Session[]>([]),[selected,setSelected]=useState(''),[node,setNode]=useState(''),[inputRecording,setInputRecording]=useState(false),[error,setError]=useState(''),[writer,setWriter]=useState(false),[reconnect,setReconnect]=useState(0);
 const host=useRef<HTMLDivElement>(null),token=useRef('');
 async function command(operation:string,params:Record<string,unknown>={},id=selected){return api(`runs/${run.id}/terminal`,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify({operation,...(id?{sessionId:id}:{}),params})});}
 useEffect(()=>{let live=true;async function refresh(){try{const r=await command('list');if(live)setSessions(r.items);}catch(e){if(live)setError((e as Error).message);}}void refresh();const timer=setInterval(refresh,2500);return()=>{live=false;clearInterval(timer);};},[run.id,csrf]);
 useEffect(()=>{if(!selected||!host.current)return;let disposed=false,ws:WebSocket|null=null,sequence='0',pending=false;
 token.current='';setWriter(false);setError('');
 const term=new Terminal({convertEol:false,scrollback:3000,allowProposedApi:false,disableStdin:false,allowTransparency:false});const fit=new FitAddon();term.loadAddon(fit);term.open(host.current);fit.fit();
 // No HTML, link, clipboard, or OSC integration is installed.
 const input=term.onData(data=>{if(!token.current)return;const bytes=new TextEncoder().encode(data);if(bytes.length>1024){setError('Input is limited to 1024 bytes per message.');return;}const base64=btoa(String.fromCharCode(...bytes));void command('input',{token:token.current,base64}).catch(e=>setError(e.message));});
 const resize=()=>{fit.fit();if(token.current&&sessions.find(s=>s.id===selected)?.kind!=='serial')void command('resize',{token:token.current,rows:term.rows,columns:term.cols}).catch(e=>setError(e.message));};window.addEventListener('resize',resize);
 const connect=()=>{ws=new WebSocket(`${location.protocol==='https:'?'wss:':'ws:'}//${location.host}/api/v1/terminal`,'graphlab.terminal.v1');ws.binaryType='arraybuffer';ws.onclose=()=>{if(!disposed)setError('Console disconnected. Reconnect to replay retained output.');};ws.onerror=()=>{if(!disposed)setError('Console connection failed.');};ws.onmessage=event=>{if(disposed)return;if(event.data instanceof ArrayBuffer){const view=new DataView(event.data);if(view.byteLength<24)return;const type=view.getUint32(16),length=view.getUint32(20);if(length!==view.byteLength-24)return;if(type===1)term.write(new Uint8Array(event.data,24));}else{const message=JSON.parse(event.data);pending=false;if(message.status>=400)setError(message.result?.error?.code??message.error??'Console request failed');else if(message.result.next)sequence=message.result.next;}};};connect();
 const poll=setInterval(()=>{if(ws?.readyState===WebSocket.OPEN&&!pending){pending=true;ws.send(JSON.stringify({runId:run.id,sessionId:selected,operation:'replay',params:{sequence},csrf}));}},250);
 const renew=setInterval(()=>{if(token.current)void command('renew-writer',{token:token.current}).catch(e=>{token.current='';setWriter(false);setError(e.message);});},5000);
 return()=>{disposed=true;clearInterval(poll);clearInterval(renew);ws?.close();input.dispose();window.removeEventListener('resize',resize);term.dispose();token.current='';};
 },[selected,run.id,csrf,reconnect]);
 async function acquire(takeover:boolean){try{const r=await command('acquire',{takeover});token.current=r.token;setWriter(true);setError('');}catch(e){setError((e as Error).message);}}
 async function open(){try{const r=await api(`runs/${run.id}/terminal`,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify({operation:'open',node,params:{recordInput:inputRecording}})});setSessions(current=>[...current,{id:r.id,node,kind:run.resources?.find(resource=>resource.logical===node)?.kind==='qemu'?'ssh':'docker',recordInput:r.recordInput}]);setSelected(r.id);}catch(e){setError((e as Error).message);}}
 return <section aria-label="Recorded consoles"><h3>Recorded consoles</h3><p>Output is recorded and may contain echoed secrets. Writer access expires after 15 seconds without renewal. Serial coverage begins at attachment.</p>
 <label>Workload <select aria-label="Workload" value={node} onChange={e=>setNode(e.target.value)}><option value="">Select workload</option>{run.resources?.filter(r=>r.kind==='container'||r.kind==='qemu').map(r=><option key={r.logical}>{r.logical}</option>)}</select></label>
 <label><input type="checkbox" checked={inputRecording} onChange={e=>setInputRecording(e.target.checked)}/> Record exact input for the new shell</label><button disabled={!node||run.state==='destroyed'} onClick={open}>Open recorded session</button>
 <label>Console <select aria-label="Console" value={selected} onChange={e=>setSelected(e.target.value)}><option value="">Select session</option>{sessions.map(s=><option key={s.id} value={s.id}>{s.node} · {s.kind} · {s.id}</option>)}</select></label>
 {selected&&<><p>{writer?'Writer lease active':'Read-only viewer'} · output recording enabled {sessions.find(s=>s.id===selected)?.recordInput?'· EXACT INPUT RECORDING ENABLED':''}</p><button disabled={run.state==='destroyed'} onClick={()=>acquire(false)}>Acquire writer</button><button disabled={run.state==='destroyed'} onClick={()=>acquire(true)}>Take over writer</button><button onClick={()=>setReconnect(x=>x+1)}>Reconnect and replay</button><button disabled={run.state==='destroyed'||sessions.find(s=>s.id===selected)?.kind==='serial'} onClick={()=>void command('close').catch(e=>setError(e.message))}>Close shell</button></>}
 {error&&<p role="alert">{error}</p>}<div ref={host} style={{height:320,background:'#101418'}}/>
 </section>;
}
