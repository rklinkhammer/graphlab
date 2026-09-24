import {test,expect} from '@playwright/test';
import {execFileSync,spawn,type ChildProcess} from 'node:child_process';
import {resolve} from 'node:path';
test('source pause/resume through Linux generator, capture, authenticated API and console',async({page,request})=>{
 test.skip(!process.env.GRAPHLAB_SOURCE_CONTROL_LIVE,'Requires isolated source control services');test.setTimeout(180000);page.setDefaultTimeout(10000);
 const config=`${process.env.HOME}/.lima/graphlab/ssh.config`,root='/var/tmp/gl6-source-20260923',base='http://127.0.0.1:18096';
 const ssh=(cmd:string)=>execFileSync('ssh',['-F',config,'-o','BatchMode=yes','lima-graphlab',cmd],{encoding:'utf8'});
 const password=ssh(`cat ${root}/password`).trim();let tunnel:ChildProcess|undefined;
 try {
 tunnel=spawn('ssh',['-F',config,'-N','-o','ExitOnForwardFailure=yes','-L','127.0.0.1:18096:127.0.0.1:18096','lima-graphlab'],{stdio:'ignore'});
 await expect.poll(async()=>{try{return(await fetch(base)).status;}catch{return 0;}}).toBe(200);
 await page.goto(base);await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await expect(page.getByRole('heading',{name:'source-controls',exact:true})).toBeVisible();
 const setupSession=await(await page.request.get(`${base}/api/v1/session`)).json();
 for(const old of (await(await page.request.get(`${base}/api/v1/runs`)).json()).items.filter((r:any)=>r.state!=='destroyed')) {
   const result=await page.request.post(`${base}/api/v1/runs/${old.id}/operations`,{headers:{Origin:base,'X-CSRF-Token':setupSession.csrf},data:{operation:old.state==='reconciling'?'recover':'destroy',expectedRevision:old.revision,idempotencyKey:crypto.randomUUID()}});expect(result.status()).toBe(202);const job=await result.json();
   await expect.poll(async()=>(await(await page.request.get(`${base}/api/v1/jobs/${job.jobId}`)).json()).state,{timeout:30000}).toBe('succeeded');
 }
 await page.route('**/api/v1/runs',async route=>{if(route.request().method()!=='POST')return route.continue();await route.continue({postData:JSON.stringify({...route.request().postDataJSON(),capturePolicy:{runBytes:67108864,reserveBytes:1048576,rotateBytes:1048576,rotateSeconds:60}})});});
 await page.getByRole('button',{name:'Start selected topology'}).click();await expect(page.getByRole('status').filter({hasText:'start: succeeded'})).toBeVisible({timeout:60000});
 const runId=(await(await page.request.get(`${base}/api/v1/runs`)).json()).items.find((r:any)=>r.state!=='destroyed').id;
 const endpoint=`${base}/api/v1/runs/${runId}/source-controls`,{csrf}=await(await page.request.get(`${base}/api/v1/session`)).json(),headers={Origin:base,'X-CSRF-Token':csrf};
 const query=async(node='b')=>(await page.request.post(`${endpoint}/query`,{data:{node},headers})).json();
 const initial=await query();expect(initial.capability.sources).toHaveLength(2);
 const status=()=>JSON.parse(ssh(`sudo docker exec ${initial.instance} /usr/local/bin/lab-node control status`));
 const counts=()=>Object.fromEntries(status().sourceControl.sources.map((s:any)=>[s.id,Number(s.generatedDatagrams)]));
 const cmd=(id:string,action='pause')=>({apiVersion:'graphlab.source-control/v1',node:'b',instance:initial.instance,epoch:initial.capability.epoch,source:'alpha',action,requestId:`${runId}-${id}`});
 const post=async(c:any)=>(await page.request.post(`${endpoint}/command`,{data:c,headers}));
 expect((await request.post(`${endpoint}/command`,{data:cmd('unauth'),headers:{Origin:base}})).status()).toBe(401);
 expect((await page.request.post(`${endpoint}/command`,{data:cmd('csrf'),headers:{Origin:base}})).status()).toBe(403);
 expect((await page.request.post(`${endpoint}/command`,{data:cmd('origin'),headers:{...headers,Origin:'http://wrong.invalid'}})).status()).toBe(403);
 expect((await post({...cmd('wrong'),instance:'c'.repeat(64)})).status()).toBe(409);
 expect((await post({...cmd('epoch'),epoch:'d'.repeat(32)})).status()).toBe(409);
 expect((await query('a')).capability).toBeNull();
 console.log('Authorization and identity checks passed');
 const captureBefore=ssh('ps -C lab-capture -o pid=,args=');expect(captureBefore.trim()).not.toBe('');
 await page.getByText('Accessible inventory · all nodes and data edges').click();await page.getByRole('button',{name:/^b · docker · runtime/}).click();
 console.log('Selected source node');
 const panel=page.getByRole('region',{name:'Source controls'});
 // Exercise real admission rejection through the UI before the successful pause.
 let rejectFirst=true;
 await page.route('**/source-controls/command',async route=>{
   if(rejectFirst){rejectFirst=false;await route.continue({postData:JSON.stringify({...route.request().postDataJSON(),epoch:'0'.repeat(32)})});}
   else await route.continue();
 });
 await panel.getByRole('button',{name:'Pause source alpha',exact:true}).click();
 await expect(panel.getByRole('status')).toContainText('failed');
 await expect(panel.getByRole('alert')).toContainText('source epoch changed');
 await expect(panel.getByRole('button',{name:'Pause source alpha',exact:true})).toBeEnabled({timeout:10000});
 console.log('Real epoch rejection recovered to fresh advertised controls');
 await panel.getByRole('button',{name:'Pause source alpha',exact:true}).click();await expect(panel.getByRole('status')).toContainText('acknowledged',{timeout:20000});
 const paused=await query(),record=paused.commands.find((c:any)=>c.request.action==='pause');expect((await(await post(record.request)).json()).outcome).toBe('acknowledged');expect((await post({...record.request,action:'resume'})).status()).toBe(409);
 const first=counts();const begin=Number(ssh('date +%s%N').trim())/1e9;
 await new Promise(r=>setTimeout(r,2200));const end=Number(ssh('date +%s%N').trim())/1e9;const second=counts();expect(second.alpha).toBe(first.alpha);expect(second.beta).toBeGreaterThan(first.beta);
 expect(ssh('ps -C lab-capture -o pid=,args=')).toBe(captureBefore);
 const detail=await(await page.request.get(`${base}/api/v1/runs/${runId}`)).json();const a=detail.health.nodes.find((n:any)=>n.id==='a').containerId;
 expect(JSON.parse(ssh(`sudo docker exec ${a} /usr/local/bin/lab-node control probe 10.231.17.2`)).probe).toBe('received');
 await panel.screenshot({path:resolve('../../docs/validation/source-control-live.png')});
 await panel.getByRole('button',{name:'Resume source alpha',exact:true}).click();await expect(panel.getByRole('status')).toContainText('resume — acknowledged',{timeout:20000});await expect.poll(()=>counts().alpha).toBeGreaterThan(second.alpha);
 await page.getByRole('button',{name:'Quiesce',exact:true}).click();await expect(page.getByRole('status').filter({hasText:'stop: succeeded'})).toBeVisible({timeout:30000});
 const held=counts();const requestBody=cmd('held-resume','resume');expect((await post(requestBody)).status()).toBe(202);await expect.poll(async()=> (await query()).commands.find((c:any)=>c.request.requestId===requestBody.requestId)?.outcome).toBe('acknowledged');await new Promise(r=>setTimeout(r,500));expect(counts()).toEqual(held);expect(status().state).toBe('held');
 // Independent decoder inspects retained PCAPNG packet bytes in the paused interval.
 const proof=JSON.parse(ssh(`sudo python3 /tmp/graphlab-source-20260923/tests/qualification/source_capture.py ${root}/state/captures ${runId} ${begin} ${end}`));expect(proof.alpha).toBe(0);expect(proof.beta).toBeGreaterThan(0);
 ssh('sudo systemctl stop graphlab-source-agent');ssh(`sudo systemd-run --unit=graphlab-source-agent --property=KillMode=process /tmp/graphlab-source-20260923/build/dev/lab-agent --socket ${root}/rpc/agent.sock --topologies ${root} --lock ${root}/artifacts.lock.json --state ${root}/state --allow-uid 501`);
 await expect.poll(async()=>{try{return(await query()).commands.find((c:any)=>c.request.requestId===requestBody.requestId)?.outcome;}catch{return'';}}).toBe('acknowledged');expect(status().state).toBe('held');
 console.log(JSON.stringify({runId,first,second,held,captureProcessesUnchanged:true,receivingWhilePaused:true,proof}));
 } finally {try{if(await page.getByRole('button',{name:'Destroy run',exact:true}).isVisible()){await page.getByRole('button',{name:'Destroy run',exact:true}).click();await expect(page.getByRole('status').filter({hasText:'destroy: succeeded'})).toBeVisible({timeout:30000});}}finally{tunnel?.kill();}}
});
