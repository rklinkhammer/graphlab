import {test,expect} from '@playwright/test';
import {execFileSync,spawn} from 'node:child_process';
import {writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
const source=process.env.GRAPHLAB_LINUX_SOURCE??'/tmp/graphlab-m0-source',root=process.env.GRAPHLAB_LIVE_ROOT??'/var/tmp/gl6-services';
const ssh=['-F',process.env.GRAPHLAB_SSH_CONFIG??`${process.env.HOME}/.lima/graphlab/ssh.config`,'lima-graphlab'];
const remote=(command:string,input?:string)=>execFileSync('ssh',[...ssh,command],{encoding:'utf8',input});
const control=(method:string,params:any)=>{
 remote(`cat > ${root}/params.json`,JSON.stringify(params));
 return JSON.parse(remote(`${source}/build/dev/lab control --socket ${root}/rpc/agent.sock --agent-uid 0 ${method} ${root}/params.json`));
};
const service=(mode:string)=>remote(`sh ${source}/qualification/live-services.sh ${mode} ${source} ${root}`);
async function settled(id:string){for(let i=0;i<600;i++){const j=control('job',{id});if(!['queued','running'].includes(j.state))return j;await new Promise(r=>setTimeout(r,100));}throw Error('job timeout');}
test('M6 real CLI/UI faults and independent API/agent restarts while recording',async({page,context})=>{
 test.skip(!process.env.GRAPHLAB_M6_LIVE,'Explicit dedicated Linux fixture required');test.setTimeout(240000);
 const password=remote(`cat ${root}/password`).trim();
 const login=async()=>{await page.goto('http://127.0.0.1:18089/');await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();await expect(page.getByRole('navigation')).toBeVisible();};
 await login();await page.getByRole('button',{name:'Start selected topology'}).click();await expect(page.getByRole('status').filter({hasText:'start: succeeded'})).toBeVisible({timeout:60000});
 let run=control('runs',{}).items.find((r:any)=>r.state==='ready');expect(run).toBeTruthy();
 run=control('run',{id:run.id});expect(run.captureCoverage).toBe('recording');
 const panel=page.getByRole('region',{name:'Telemetry and faults'});
 await panel.getByRole('button',{name:'a-s',exact:true}).click();await panel.getByLabel('Duration seconds').fill('30');await panel.getByRole('button',{name:'Preview fault placement'}).click();
 let submitted:any,cliResult:any;
 await page.route(`**/api/v1/runs/${run.id}/faults`,async route=>{
  if(route.request().method()!=='POST'){await route.continue();return;}
  const body=route.request().postDataJSON();submitted={...body,runId:run.id,idempotencyKey:'m6-concurrent-cli-'+run.id,fault:{...body.fault,direction:'b-to-a'}};
  remote(`cat > ${root}/concurrent.json`,JSON.stringify(submitted));
  const result=new Promise<any>(done=>{const child=spawn('ssh',[...ssh,`${source}/build/dev/lab control --socket ${root}/rpc/agent.sock --agent-uid 0 fault.apply ${root}/concurrent.json`]);let out='',err='';child.stdout.on('data',b=>out+=b);child.stderr.on('data',b=>err+=b);child.on('close',code=>done({code,out,err}));});
  await route.continue();cliResult=await result;
 });
 const response=page.waitForResponse(r=>r.url().endsWith(`/runs/${run.id}/faults`)&&r.request().method()==='POST');
 await panel.getByRole('button',{name:'Apply directional fault'}).click();const http=await response;
 await expect.poll(()=>Boolean(cliResult)).toBe(true);
 const ui=await http.json();expect([202,409]).toContain(http.status());expect((http.status()===202?1:0)+(cliResult.code===0?1:0)).toBe(1);
 await page.unroute(`**/api/v1/runs/${run.id}/faults`);
 const admitted=cliResult.code===0?JSON.parse(cliResult.out):ui;
 expect((await settled(admitted.jobId)).state).toBe('succeeded');
 // Explicit same-key replay through the real CLI, then conflicting body/stale revision.
 run=control('run',{id:run.id});const unique={runId:run.id,expectedRevision:run.revision,idempotencyKey:'m6-cli-idempotent-'+run.id,fault:{edge:'b-s',kind:'netem',direction:'a-to-b',delayMs:1,durationSeconds:30}};
 const once=control('fault.apply',unique);expect(control('fault.apply',unique).jobId).toBe(once.jobId);expect((await settled(once.jobId)).state).toBe('succeeded');
 expect(()=>control('fault.apply',{...unique,fault:{...unique.fault,delayMs:2}})).toThrow();expect(()=>control('fault.apply',{...unique,idempotencyKey:'m6-stale-new-key-'+run.id})).toThrow();
 // Remove policies before continuity test.
 for(const [id,f] of Object.entries(control('faults',{runId:run.id}).items) as any){if(f.state!=='active')continue;run=control('run',{id:run.id});expect((await settled(control('fault.remove',{runId:run.id,faultId:id,expectedRevision:run.revision,idempotencyKey:`m6-remove-${id}`}).jobId)).state).toBe('succeeded');}
 const session=control('terminal',{runId:run.id,node:'a',operation:'open',owner:'m6-live',params:{}});
 const terminal=(operation:string,params:any={})=>control('terminal',{runId:run.id,sessionId:session.id,owner:'m6-live',operation,params});
 await expect.poll(()=>terminal('status').sourceReady).toBe(true);
 const token=terminal('acquire').token;
 terminal('input',{token,base64:Buffer.from("i=0; while [ $i -lt 60 ]; do echo M6_CONTINUITY_$i; i=$((i+1)); sleep 0.1; done\n").toString('base64')});
 const before=control('run',{id:run.id});
 service('stop-api');await new Promise(r=>setTimeout(r,600));service('api');
 await expect.poll(async()=>{try{return (await page.request.get('http://127.0.0.1:18089/api/v1/runs')).status();}catch{return 0;}}).toBe(401);
 await context.clearCookies();await login();
 const afterApi=control('run',{id:run.id});expect(afterApi.captures.map((c:any)=>c.id)).toEqual(before.captures.map((c:any)=>c.id));expect(afterApi.controllerGeneration).toBe(before.controllerGeneration);
 service('stop-agent');await new Promise(r=>setTimeout(r,600));service('agent');
 await expect.poll(()=>{try{return control('run',{id:run.id}).controllerGeneration;}catch{return before.controllerGeneration;}}).not.toBe(before.controllerGeneration);
 const after=control('run',{id:run.id});expect(after.state).toBe('reconciling');expect(after.captures.map((c:any)=>c.id)).toEqual(before.captures.map((c:any)=>c.id));expect(new Set(after.sessions.map((s:any)=>s.id)).size).toBe(after.sessions.length);
 expect(after.captureObservations.map((c:any)=>c.invocationId)).toEqual(before.captureObservations.map((c:any)=>c.invocationId));
 expect(()=>terminal('input',{token,base64:Buffer.from('echo STALE\n').toString('base64')})).toThrow();
 let output='';let sequence='0';for(let i=0;i<20;i++){const replay=terminal('replay',{sequence});for(const r of replay.records)if(r.type===1)output+=Buffer.from(r.base64,'base64').toString();sequence=replay.next;await new Promise(r=>setTimeout(r,200));}
 expect(output).toContain('M6_CONTINUITY_0');expect(output).toContain('M6_CONTINUITY_59');
 // A real worker failure must lose coverage, rather than inheriting continuity.
 remote(`sudo systemctl kill --signal=KILL ${after.captures[0].unit}`);
 service('stop-agent');service('agent');await expect.poll(()=>{try{return control('run',{id:run.id}).captureCoverage;}catch{return '';}}).toBe('incomplete');
 run=control('run',{id:run.id});const cleanup=control('operate',{runId:run.id,expectedRevision:run.revision,operation:'recover',idempotencyKey:'m6-service-cleanup-'+run.id});expect((await settled(cleanup.jobId)).state).toBe('succeeded');
 writeFileSync(resolve(import.meta.dirname,'../../../build/m6-service-proof.json'),JSON.stringify({before,afterApi,after,concurrency:{httpStatus:http.status(),cliCode:cliResult.code},terminalLastSequence:sequence,cleanup},null,2));
});
