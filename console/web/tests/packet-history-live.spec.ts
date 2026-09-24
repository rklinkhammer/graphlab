import {test, expect} from "@playwright/test";
import {execFileSync, spawn, type ChildProcess} from "node:child_process";
import {readFileSync} from "node:fs";
import {createHash} from "node:crypto";
import {resolve} from "node:path";

test("finalized Linux PCAPNG to authenticated packet history and browser", async ({page, request}) => {
  const maintenance = Boolean(process.env.GRAPHLAB_PACKET_MAINTENANCE_LIVE);
  test.skip(!process.env.GRAPHLAB_PACKETS_LIVE && !maintenance, "Requires isolated Linux packet-history fixture");
  test.setTimeout(180000);
  const config = process.env.GRAPHLAB_SSH_CONFIG ?? `${process.env.HOME}/.lima/graphlab/ssh.config`;
  const root = maintenance ? "/var/tmp/gl6-maint-20260923" : "/var/tmp/gl6-packets-20260923", source = maintenance ? "/tmp/graphlab-maintenance-20260923" : "/tmp/graphlab-packets-20260923";
  const port = maintenance ? 18094 : 18093, unit = maintenance ? "graphlab-maintenance-agent" : "graphlab-packets-agent";
  const ssh = (cmd: string) => execFileSync("ssh", ["-F",config,"-o","BatchMode=yes","lima-graphlab",cmd], {encoding:"utf8"});
  const password = ssh(`cat ${root}/password`).trim(), base = `http://127.0.0.1:${port}`;
  let tunnel: ChildProcess | undefined, runId = "", verified = false;
  try {
    tunnel = spawn("ssh",["-F",config,"-N","-o","ExitOnForwardFailure=yes","-L",`127.0.0.1:${port}:127.0.0.1:${port}`,"lima-graphlab"],{stdio:"ignore"});
    await expect.poll(async () => {try{return (await fetch(base)).status;}catch{return 0;}}).toBe(200);
    await page.goto(base);
    await page.getByLabel("Operator credential").fill(password);
    await page.getByRole("button",{name:"Sign in",exact:true}).click();
    await page.route("**/api/v1/runs", async route => {
      if(route.request().method()!=="POST")return route.continue();
      await route.continue({postData:JSON.stringify({...route.request().postDataJSON(),capturePolicy:{runBytes:67108864,reserveBytes:1048576,rotateBytes:1048576,rotateSeconds:60}})});
    });
    await page.getByRole("button",{name:"Start selected topology"}).click();
    await expect(page.getByRole("status").filter({hasText:"start: succeeded"})).toBeVisible({timeout:60000});
    const list = await (await page.request.get(`${base}/api/v1/runs`)).json();
    runId = list.items.find((r:any) => r.state!=="destroyed").id;
    const endpoint = `${base}/api/v1/runs/${runId}/packet-history`;
    const {csrf} = await (await page.request.get(`${base}/api/v1/session`)).json();
    const headers = {Origin:base,"X-CSRF-Token":csrf};
    const query = async (data:any={}) => (await page.request.post(`${endpoint}/query`,{data,headers})).json();
    expect((await request.get(endpoint)).status()).toBe(401);
    expect((await page.request.post(`${endpoint}/query`,{data:{},headers:{Origin:base}})).status()).toBe(403);
    expect((await page.request.post(`${endpoint}/query`,{data:{},headers:{...headers,Origin:"http://hostile.invalid"}})).status()).toBe(403);
    expect((await page.request.post(`${endpoint}/query`,{data:{limit:201},headers})).status()).toBe(422);
    if(maintenance){
      expect((await request.post(`${endpoint}/rebuild`,{data:{},headers:{Origin:base}})).status()).toBe(401);
      expect((await page.request.post(`${endpoint}/rebuild`,{data:{},headers:{Origin:base}})).status()).toBe(403);
      expect((await page.request.post(`${endpoint}/recover`,{data:{scope:"all-runs"},headers:{...headers,Origin:"http://hostile.invalid"}})).status()).toBe(403);
      expect((await page.request.post(`${endpoint}/recover`,{data:{scope:"all-runs"},headers})).status()).toBe(409);
    }
    expect((await query()).items).toEqual([]);
    await expect.poll(async () => {
      const r=await (await page.request.get(`${base}/api/v1/runs/${runId}`)).json();
      return r.health?.nodes?.find((n:any)=>n.id==="b")?.containerId ?? "";
    },{timeout:15000}).toMatch(/^[a-f0-9]{64}$/);
    const run = await (await page.request.get(`${base}/api/v1/runs/${runId}`)).json();
    const container=run.health.nodes.find((n:any)=>n.id==="b").containerId;
    for(let i=0;i<10;i++)expect(JSON.parse(ssh(`sudo docker exec ${container} /usr/local/bin/lab-node control probe 10.231.17.1`)).probe).toBe("received");
    expect((await query()).items).toEqual([]); // traffic exists, but the segment is still active
    await page.getByRole("button",{name:"Quiesce",exact:true}).click();
    await expect(page.getByRole("status").filter({hasText:"stop: succeeded"})).toBeVisible({timeout:30000});
    const filter={edge:"a-s",protocol:"udp",limit:200};
    await expect.poll(async()=> (await query(filter)).items.length,{timeout:30000}).toBe(20);
    const indexed=await query(filter);
    expect(indexed.indexError).toBeNull();
    expect(indexed.items.every((p:any)=>p.direction==="unknown" && p.headers.decodeStatus==="complete")).toBe(true);
    expect((await query({...filter,node:"b"})).items).toEqual([]);
    expect((await query({...filter,node:"a"})).items).toHaveLength(20);
    const first=await query({...filter,limit:5});
    expect(first.nextCursor).toBeTruthy();
    const older=await query({...filter,limit:5,cursor:first.nextCursor});
    expect(older.items).toHaveLength(5);
    expect(older.items.some((p:any)=>first.items.some((x:any)=>x.id===p.id))).toBe(false);
    expect((await page.request.post(`${endpoint}/query`,{data:{...filter,edge:"b-s",cursor:first.nextCursor},headers})).status()).toBe(409);
    await page.getByRole("region",{name:"Telemetry and faults"}).getByRole("button",{name:"a-s",exact:true}).click();
    const panel=page.getByRole("region",{name:"Packet history"});
    await panel.getByLabel("Packet protocol").selectOption("udp");
    await expect(panel.getByRole("button",{name:/Show capture/})).toHaveCount(20);
    const artifact=indexed.items[0].artifactId;
    await panel.getByRole("button",{name:`Show capture ${artifact}`,exact:true}).first().click();
    const downloadEvent=page.waitForEvent("download");
    await page.locator(`#artifact-${artifact}`).getByRole("button",{name:/Download PCAPNG/}).click();
    const downloaded=await downloadEvent, file=resolve("..","..","build","packet-history-live.pcapng");
    await downloaded.saveAs(file);
    const bytes=readFileSync(file);
    expect(`sha256:${createHash("sha256").update(bytes).digest("hex")}`).toBe(indexed.items[0].artifactSha256);
    for(const p of indexed.items){
      const n=Number(p.blockOffset);
      expect(bytes.readUInt32LE(n)).toBe(6);
      expect(bytes.readUInt32LE(n+20)).toBe(p.capturedLength);
      expect(bytes.readUInt32LE(n+24)).toBe(p.originalLength);
      expect(((BigInt(bytes.readUInt32LE(n+12))<<32n)|BigInt(bytes.readUInt32LE(n+16))).toString()).toBe(p.timestampUnixMicros);
    }
    const cap=run.captures.find((c:any)=>c.id===indexed.items[0].captureId);
    expect(cap.id).toMatch(/^[a-f0-9]{24}$/);
    const tcpdump=ssh(`sudo tcpdump -tt -n -r ${root}/state/captures/${cap.id}/0.pcapng udp`);
    expect(tcpdump.trim().split("\n")).toHaveLength(20);
    await panel.screenshot({path:resolve("..","..",maintenance ? "docs/validation/packet-maintenance-live.png" : "docs/validation/packet-history-live.png")});
    ssh(`sudo systemctl stop ${unit}`);
    ssh(`sudo systemd-run --unit=${unit} --property=KillMode=process ${source}/build/dev/lab-agent --socket ${root}/rpc/agent.sock --topologies ${root} --lock ${root}/artifacts.lock.json --state ${root}/state --allow-uid 501`);
    await expect.poll(async()=>{try{return (await query({...filter,limit:5,cursor:first.nextCursor})).items?.map((p:any)=>p.id);}catch{return [];}}).toEqual(older.items.map((p:any)=>p.id));
    if(maintenance){
      await panel.getByText("Packet index maintenance",{exact:true}).click();
      await panel.getByRole("button",{name:"Rebuild this run's packet index",exact:true}).click();
      await expect.poll(async()=> (await query()).maintenance?.state,{timeout:30000}).toBe("completed");
      expect((await query(filter)).items).toHaveLength(20);
      expect((await page.request.post(`${endpoint}/query`,{data:{...filter,cursor:first.nextCursor},headers})).status()).toBe(409);
      expect((await page.request.post(`${endpoint}/recover`,{data:{},headers})).status()).toBe(422);
      await panel.getByLabel("Replace derived packet indexes for all runs; preserve capture files.").check();
      await panel.getByRole("button",{name:"Recover packet index for all runs",exact:true}).click();
      await expect(panel.getByRole("status")).toContainText("recover: completed");
      await expect.poll(async()=> (await query(filter)).items.length,{timeout:30000}).toBe(20);
      expect((await query(filter)).items[0].artifactSha256).toBe(indexed.items[0].artifactSha256);
      expect((await page.request.post(`${endpoint}/recover`,{data:{scope:"all-runs"},headers})).status()).toBe(409);
      await expect(panel.getByRole("button",{name:/Show capture/})).toHaveCount(20,{timeout:15000});
      await panel.screenshot({path:resolve("..","..","docs/validation/packet-maintenance-live.png")});
      console.log("Maintenance: authenticated rebuild/recovery, running-run recovery rejection, cursor invalidation, quarantine non-overwrite and capture SHA preservation passed");
    }
    console.log(JSON.stringify({runId,edge:"a-s",udpPackets:indexed.items.length,tcpdumpUdpPackets:20,artifact,sha256:indexed.items[0].artifactSha256,exactOffsetsAndTimestamps:true,restartCursorPreserved:true,segments:indexed.segments}));
    verified = true;
  } finally {
    try {
      if(await page.getByRole("button",{name:"Destroy run",exact:true}).isVisible()){
        await page.getByRole("button",{name:"Destroy run",exact:true}).click();
        await expect(page.getByRole("status").filter({hasText:"destroy: succeeded"})).toBeVisible({timeout:30000});
        if(verified){const retained=await (await page.request.get(`${base}/api/v1/runs/${runId}/packet-history`)).json();expect(retained.items.length).toBeGreaterThan(0);}
      }
    }finally{tunnel?.kill();}
  }
});
