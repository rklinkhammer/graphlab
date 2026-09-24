import {test,expect} from '@playwright/test';
import {execFileSync,spawn} from 'node:child_process';
import {resolve} from 'node:path';
test('message fixture endpoint reports correlate to verified Linux PCAPNG identities',async({page,request})=>{
 test.skip(!process.env.GRAPHLAB_MESSAGES_LIVE,'Requires dedicated message services');test.setTimeout(180000);
 const root=process.env.GRAPHLAB_LIVE_ROOT??'/var/tmp/gl6-messages-20260924',base='http://127.0.0.1:18099',config=`${process.env.HOME}/.lima/graphlab/ssh.config`;
 const ssh=(cmd:string)=>execFileSync('ssh',['-F',config,'lima-graphlab',cmd],{encoding:'utf8'});
 const tunnel=spawn('ssh',['-F',config,'-N','-L','18099:127.0.0.1:18099','lima-graphlab'],{stdio:'ignore'});let runId='',headers:any;
 async function operation(action:string){const r=await(await page.request.get(`${base}/api/v1/runs/${runId}`)).json(),j=await(await page.request.post(`${base}/api/v1/runs/${runId}/operations`,{headers,data:{operation:action,expectedRevision:r.revision,idempotencyKey:crypto.randomUUID()}})).json();await expect.poll(async()=>(await(await page.request.get(`${base}/api/v1/jobs/${j.jobId}`)).json()).state,{timeout:30000}).toBe('succeeded');}
 try{
 await expect.poll(async()=>{try{return(await fetch(base)).status;}catch{return 0;}}).toBe(200);
 await page.goto(base);await page.getByLabel('Operator credential').fill(ssh(`cat ${root}/password`).trim());await page.getByRole('button',{name:'Sign in',exact:true}).click();await expect(page.getByRole('heading',{name:'messages',exact:true})).toBeVisible();
 headers={Origin:base,'X-CSRF-Token':(await(await page.request.get(`${base}/api/v1/session`)).json()).csrf};
 for(const r of (await(await page.request.get(`${base}/api/v1/runs`)).json()).items.filter((r:any)=>r.state!=='destroyed')){runId=r.id;await operation(r.state==='reconciling'?'recover':'destroy');}runId='';
 const admission=await(await page.request.post(`${base}/api/v1/runs`,{headers,data:{topologyHash:(await(await page.request.get(`${base}/api/v1/topologies`)).json()).items[0].hash,idempotencyKey:crypto.randomUUID(),capturePolicy:{runBytes:67108864,reserveBytes:1048576,rotateBytes:1048576,rotateSeconds:60}}})).json();
 runId=admission.runId;await expect.poll(async()=>(await(await page.request.get(`${base}/api/v1/jobs/${admission.jobId}`)).json()).state,{timeout:60000}).toBe('succeeded');
 const endpoint=`${base}/api/v1/runs/${runId}/messages`,query=async(node='b')=>(await page.request.post(`${endpoint}/query`,{headers,data:{node}})).json();
 await expect.poll(async()=>(await query()).items?.length||0,{timeout:30000}).toBeGreaterThan(0);
 await expect.poll(async()=>(await query('a')).items?.length||0,{timeout:30000}).toBeGreaterThan(0);
 await operation('stop');
 const detail=await(await page.request.get(`${base}/api/v1/runs/${runId}`)).json();
 const report=(node:string)=>{const resource=detail.resources.find((r:any)=>r.logical===node&&r.kind==='container');return JSON.parse(ssh(`sudo docker exec ${resource.identity.id} /usr/local/bin/lab-node control message-observations`)).messageObservations;};
 const source=report('b'),target=report('a');let event:any;
 await expect.poll(async()=>{event=(await query()).items.find((e:any)=>e.observation.kind==='send'&&e.observation.phase==='request'&&source.events.some((s:any)=>JSON.stringify(s)===JSON.stringify(e.observation)));return Boolean(event);},{timeout:20000}).toBe(true);
 const received=target.events.find((e:any)=>e.kind==='receive'&&e.phase==='request'&&e.messageId===event.observation.messageId);expect(received).toBeTruthy();expect(source.payloadRecorded).toBe(false);
 const correlate=async(data:any)=>page.request.post(`${endpoint}/correlate`,{headers,data});
 const exact=await(await correlate({node:'b',id:event.id,edge:'b-s'})).json();expect(exact.status).toBe('exact');expect(exact.matches).toHaveLength(1);
 const m=exact.matches[0],proof=JSON.parse(ssh(`sudo python3 ${process.env.GRAPHLAB_LIVE_SOURCE??'/tmp/graphlab-messages-20260924'}/tests/qualification/message_capture.py ${root}/state/captures ${runId} '${JSON.stringify(event)}' ${m.artifactId} ${m.blockOffset} ${m.packetIndex}`));expect(proof.wireIdentifierVerified).toBe(true);
 const union=await(await correlate({node:'b',id:event.id})).json();expect(union.status).toBe('ambiguous');expect(union.matches.length).toBeGreaterThan(1);
 expect((await request.post(`${endpoint}/query`,{headers:{Origin:base},data:{node:'b'}})).status()).toBe(401);expect((await page.request.post(`${endpoint}/query`,{headers:{Origin:base},data:{node:'b'}})).status()).toBe(403);expect((await page.request.post(`${endpoint}/query`,{headers:{...headers,Origin:'http://wrong.invalid'},data:{node:'b'}})).status()).toBe(403);expect((await correlate({node:'a',id:event.id})).status()).toBe(404);
 await page.reload();await page.getByText('Accessible inventory · all nodes and data edges').click();await page.getByRole('button',{name:/^b · docker · runtime/}).click();const panel=page.getByRole('region',{name:'Application message observations'});await panel.getByRole('button',{name:`Correlate event ${event.id}`,exact:true}).click();await expect(panel).toContainText('Correlation: ambiguous');await panel.getByLabel('Correlation capture edge').selectOption('b-s');await panel.getByRole('button',{name:`Correlate event ${event.id}`,exact:true}).click();await expect(panel).toContainText('Correlation: exact');await panel.screenshot({path:resolve('../../docs/validation/messages-live.png')});
 ssh(`sudo systemctl stop ${process.env.GRAPHLAB_LIVE_AGENT??'graphlab-messages-agent'}`);ssh(`sudo systemd-run --unit=${process.env.GRAPHLAB_LIVE_AGENT??'graphlab-messages-agent'} --property=KillMode=process ${process.env.GRAPHLAB_LIVE_SOURCE??'/tmp/graphlab-messages-20260924'}/build/dev/lab-agent --socket ${root}/rpc/agent.sock --topologies ${root} --lock ${root}/artifacts.lock.json --state ${root}/state --allow-uid 501`);
 await expect.poll(async()=>{try{return(await query()).items.some((e:any)=>e.id===event.id);}catch{return false;}}).toBe(true);await operation('destroy');expect((await(await correlate({node:'b',id:event.id,edge:'b-s'})).json()).status).toBe('exact');
 console.log(JSON.stringify({runId,sourceEpoch:source.epoch,targetEpoch:target.epoch,eventId:event.id,messageId:event.observation.messageId,independentEndpointReports:true,proof,unionMatches:union.matches.length,authorization:true,restart:true,retainedAfterDestruction:true}));
 }finally{try{if(runId){const r=await(await page.request.get(`${base}/api/v1/runs/${runId}`)).json();if(r.state!=='destroyed')await operation(r.state==='reconciling'?'recover':'destroy');}}finally{tunnel.kill();}}
});
