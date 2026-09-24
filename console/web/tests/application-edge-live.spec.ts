import {test,expect} from '@playwright/test';
import {execFileSync,spawn,type ChildProcess} from 'node:child_process';
import {resolve} from 'node:path';
test('two application streams through real Linux workload, agent, API and console', async ({page,request}) => {
  test.skip(!process.env.GRAPHLAB_APPLICATION_EDGE_LIVE,'Requires isolated application edge services');
  test.setTimeout(180000);
  const config=process.env.GRAPHLAB_SSH_CONFIG ?? `${process.env.HOME}/.lima/graphlab/ssh.config`;
  const root='/var/tmp/gl6-edge-20260923', source='/tmp/graphlab-edge-20260923', base='http://127.0.0.1:18095';
  const ssh=(cmd:string)=>execFileSync('ssh',['-F',config,'-o','BatchMode=yes','lima-graphlab',cmd],{encoding:'utf8'});
  const password=ssh(`cat ${root}/password`).trim();
  let tunnel:ChildProcess|undefined,runId='';
  try {
    tunnel=spawn('ssh',['-F',config,'-N','-o','ExitOnForwardFailure=yes','-L','127.0.0.1:18095:127.0.0.1:18095','lima-graphlab'],{stdio:'ignore'});
    await expect.poll(async()=>{try{return (await fetch(base)).status;}catch{return 0;}}).toBe(200);
    await page.goto(base);await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();
    await page.route('**/api/v1/runs',async route=>{
      if(route.request().method()!=='POST')return route.continue();
      await route.continue({postData:JSON.stringify({...route.request().postDataJSON(),capturePolicy:{runBytes:67108864,reserveBytes:1048576,rotateBytes:1048576,rotateSeconds:60}})});
    });
    await page.getByRole('button',{name:'Start selected topology'}).click();
    await expect(page.getByRole('status').filter({hasText:'start: succeeded'})).toBeVisible({timeout:60000});
    runId=(await (await page.request.get(`${base}/api/v1/runs`)).json()).items.find((r:any)=>r.state!=='destroyed').id;
    const endpoint=`${base}/api/v1/runs/${runId}/application-telemetry`;
    const {csrf}=await (await page.request.get(`${base}/api/v1/session`)).json();
    const headers={Origin:base,'X-CSRF-Token':csrf};
    const read=async(params:any={})=>(await page.request.post(`${endpoint}/query`,{data:params,headers})).json();
    expect((await request.get(endpoint)).status()).toBe(401);
    expect((await page.request.post(`${endpoint}/query`,{data:{edge:'alpha'},headers:{Origin:base}})).status()).toBe(403);
    expect((await page.request.post(`${endpoint}/query`,{data:{},headers:{...headers,Origin:'http://hostile.invalid'}})).status()).toBe(403);
    expect((await page.request.post(`${endpoint}/query`,{data:{limit:201},headers})).status()).toBe(422);
    await expect.poll(async()=> (await read({edge:'alpha'})).current.length,{timeout:20000}).toBe(1);
    const detail=await (await page.request.get(`${base}/api/v1/runs/${runId}`)).json();
    const a=detail.health.nodes.find((n:any)=>n.id==='a').containerId;
    const b=detail.health.nodes.find((n:any)=>n.id==='b');
    expect(b.gate.applicationTelemetry).toBeUndefined();expect(b.gate.applicationEdgeTelemetry).toBeUndefined();
    expect(a).toMatch(/^[a-f0-9]{64}$/);expect(b.containerId).toMatch(/^[a-f0-9]{64}$/);
    const counts={alpha:5,beta:3};
    for(const [stream,count] of Object.entries(counts))
      for(let i=0;i<count;i++)expect(JSON.parse(ssh(`sudo docker exec ${b.containerId} /usr/local/bin/lab-node control probe-${stream} 10.231.17.1`)).probe).toBe('received');
    const independent=JSON.parse(ssh(`sudo docker exec ${a} /usr/local/bin/lab-node control status`));
    for(const [stream,count] of Object.entries(counts)) {
      await expect.poll(async()=> (await read({edge:stream})).current[0]?.report.counters.receivedMessages,{timeout:20000}).toBe(String(count));
      const data=await read({edge:stream});const s=data.current[0];
      expect(data.errors).toEqual({});expect(data.current).toHaveLength(1);
      expect(s.workloadInstance).toBe(a);expect(s.node).toBe('a');expect(s.report.endpoint).toBe('target');
      expect(s.report.counters.receivedPayloadBytes).toBe(String(count*16));
      expect(s.report.counters.reconnects).toBe(String(count-1));
      expect(s.report.counters.sentMessages).toBeNull();expect(s.report.counters.backpressureNs).toBeNull();
      expect(s.latency.count).toBe(String(count));expect(s.latency.kind).toBe('local-service-time');
      expect(s.report.counters).toEqual(independent.applicationEdgeTelemetry.find((r:any)=>r.edge===stream).counters);
      expect((await read({edge:stream,node:'b'})).current).toEqual([]);
    }
    const zero=await read({node:'b'});expect(zero.current).toEqual([]);
    const unknown=await read({edge:'unknown'});expect(unknown.current).toEqual([]);
    const panel=page.getByRole('region',{name:'Application telemetry'});
    await page.getByRole('button',{name:'alpha: b → a',exact:true}).click();
    await expect(panel).toContainText('5 cumulative samples');await expect(panel).toContainText('Reconnects: 4');
    await page.getByRole('button',{name:'beta: b → a',exact:true}).click();
    await expect(panel).toContainText('3 cumulative samples');await expect(panel).not.toContainText('5 cumulative samples');
    await panel.screenshot({path:resolve('../../docs/validation/application-edge-live.png')});
    await page.getByRole('button',{name:'Quiesce',exact:true}).click();
    await expect(page.getByRole('status').filter({hasText:'stop: succeeded'})).toBeVisible({timeout:30000});
    ssh('sudo systemctl stop graphlab-edge-agent');
    ssh(`sudo systemd-run --unit=graphlab-edge-agent --property=KillMode=process ${source}/build/dev/lab-agent --socket ${root}/rpc/agent.sock --topologies ${root} --lock ${root}/artifacts.lock.json --state ${root}/state --allow-uid 501`);
    await expect.poll(async()=>{try{return (await read({edge:'alpha'})).current[0]?.stale;}catch{return false;}},{timeout:20000}).toBe(true);
    await expect(page.getByRole('button',{name:'Resume',exact:true})).toBeVisible({timeout:10000});
    await page.getByRole('button',{name:'Resume',exact:true}).click();
    await expect(page.getByRole('status').filter({hasText:'resume: succeeded'})).toBeVisible({timeout:30000});
    await expect.poll(async()=>{try{return (await read({edge:'alpha'})).items.some((s:any)=>s.gapReason==='collector_restarted');}catch{return false;}},{timeout:20000}).toBe(true);
    const persisted=await read({edge:'beta'});expect(persisted.current[0].report.counters.receivedMessages).toBe('3');
    console.log(JSON.stringify({runId,counts,bytes:{alpha:80,beta:48},reconnects:{alpha:4,beta:2},captureCoverage:detail.captureCoverage,legacyReports:zero.current.length,independentCounters:independent.applicationEdgeTelemetry.map((r:any)=>({edge:r.edge,counters:r.counters}))}));
  } finally {
    try {
      if(await page.getByRole('button',{name:'Destroy run',exact:true}).isVisible()) {
        await page.getByRole('button',{name:'Destroy run',exact:true}).click();
        await expect(page.getByRole('status').filter({hasText:'destroy: succeeded'})).toBeVisible({timeout:30000});
      }
    } finally {tunnel?.kill();}
  }
});
