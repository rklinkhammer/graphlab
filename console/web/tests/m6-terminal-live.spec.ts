import {test,expect} from '@playwright/test';
import {execFileSync} from 'node:child_process';
import {writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {connect,Socket} from 'node:net';
import {randomBytes,createHash} from 'node:crypto';
const source=process.env.GRAPHLAB_LINUX_SOURCE??'/tmp/graphlab-m0-source',root=process.env.GRAPHLAB_LIVE_ROOT??'/var/tmp/gl6-services';
const ssh=['-F',process.env.GRAPHLAB_SSH_CONFIG??`${process.env.HOME}/.lima/graphlab/ssh.config`,'lima-graphlab'];
const remote=(command:string,input?:string)=>execFileSync('ssh',[...ssh,command],{encoding:'utf8',input});
const control=(method:string,params:any)=>{remote(`cat > ${root}/params.json`,JSON.stringify(params));return JSON.parse(remote(`${source}/build/dev/lab control --socket ${root}/rpc/agent.sock --agent-uid 0 ${method} ${root}/params.json`));};
const sleep=(ms:number)=>new Promise(r=>setTimeout(r,ms));
// Minimal test peer permits pausing TCP reads, unlike a browser's eager WebSocket.
class Peer {
 socket!:Socket; buffer=Buffer.alloc(0); fragments:Buffer[]=[]; kind=0; records:any[]=[];
 waiting?:{resolve:(v:any)=>void,reject:(e:Error)=>void}; failure?:Error;
 async open(cookie:string){
  this.socket=connect(18089,'127.0.0.1');
  const key=randomBytes(16).toString('base64');
  await new Promise<void>((done,fail)=>{
   let header=Buffer.alloc(0);
   const receive=(b:Buffer)=>{header=Buffer.concat([header,b]);const end=header.indexOf('\r\n\r\n');if(end<0)return;
    this.socket.off('data',receive);const text=header.subarray(0,end).toString();
    if(!text.startsWith('HTTP/1.1 101')||!text.includes(createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'))){fail(Error('upgrade denied'));return;}
    this.socket.on('data',b=>this.consume(b));this.consume(header.subarray(end+4));done();};
   this.socket.once('error',fail);this.socket.on('data',receive);
   this.socket.once('connect',()=>this.socket.write(`GET /api/v1/terminal HTTP/1.1\r\nHost: 127.0.0.1:18089\r\nOrigin: http://127.0.0.1:18089\r\nCookie: ${cookie}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Protocol: graphlab.terminal.v1\r\n\r\n`));
  });
  this.socket.on('error',e=>this.reject(e));this.socket.on('close',()=>this.reject(Error('peer closed')));
 }
 reject(e:Error){this.failure=e;this.waiting?.reject(e);this.waiting=undefined;}
 send(value:any,opcode=1){const body=opcode===1?Buffer.from(JSON.stringify(value)):value;const mask=randomBytes(4);const h=Buffer.alloc(body.length<126?2:4);h[0]=0x80|opcode;h[1]=0x80|(body.length<126?body.length:126);if(body.length>=126)h.writeUInt16BE(body.length,2);const b=Buffer.from(body);for(let i=0;i<b.length;i++)b[i]^=mask[i%4];this.socket.write(Buffer.concat([h,mask,b]));}
 consume(bytes:Buffer){
  this.buffer=Buffer.concat([this.buffer,bytes]);
  while(this.buffer.length>=2){const b=this.buffer;const op=b[0]&15,fin=Boolean(b[0]&128);let length=b[1]&127,start=2;
   if(length===126){if(b.length<4)return;length=b.readUInt16BE(2);start=4;}else if(length===127){if(b.length<10)return;length=Number(b.readBigUInt64BE(2));start=10;}
   if(length>1048576){this.reject(Error('unbounded frame'));this.socket.destroy();return;}if(b.length<start+length)return;
   const payload=b.subarray(start,start+length);this.buffer=b.subarray(start+length);
   if(op===8){this.reject(Error('server close'));return;}if(op===9){this.send(payload,10);continue;}
   if(op!==0)this.kind=op;this.fragments.push(payload);if(!fin)continue;const message=Buffer.concat(this.fragments);this.fragments=[];
   if(this.kind===2){expect(message.length).toBeGreaterThanOrEqual(24);expect(message.readUInt32BE(20)).toBe(message.length-24);this.records.push({sequence:message.readBigUInt64BE(0).toString(),offset:message.readBigUInt64BE(8).toString(),type:message.readUInt32BE(16),base64:message.subarray(24).toString('base64')});}
   else if(this.kind===1){const result={...JSON.parse(message.toString()),records:this.records};this.records=[];const w=this.waiting;this.waiting=undefined;w?.resolve(result);}
  }
 }
 request(value:any){if(this.failure)return Promise.reject(this.failure);if(this.waiting)throw Error('one outstanding page');return new Promise<any>((resolve,reject)=>{this.waiting={resolve,reject};this.send(value);});}
 close(){this.socket.destroy();}
}
test('M6 paused TCP viewer does not block recording or independent exact replay',async({page,context})=>{
 test.skip(!process.env.GRAPHLAB_M6_LIVE,'Dedicated Linux services required');test.setTimeout(180000);
 await page.goto('http://127.0.0.1:18089/');await page.getByLabel('Operator credential').fill(remote(`cat ${root}/password`).trim());await page.getByRole('button',{name:'Sign in',exact:true}).click();await expect(page.getByRole('navigation')).toBeVisible();
 await page.getByRole('button',{name:'Start selected topology'}).click();await expect(page.getByRole('status').filter({hasText:'start: succeeded'})).toBeVisible({timeout:60000});
 const run=control('runs',{}).items.find((r:any)=>r.state==='ready');expect(run).toBeTruthy();
 const session=control('terminal',{runId:run.id,node:'a',operation:'open',owner:'t12',params:{}});
 const terminal=(operation:string,params:any={})=>control('terminal',{runId:run.id,sessionId:session.id,owner:'t12',operation,params});
 await expect.poll(()=>terminal('status').sourceReady).toBe(true);const token=terminal('acquire').token;
 const input=(s:string)=>terminal('input',{token,base64:Buffer.from(s).toString('base64')});
 input("stty -echo; printf 'WS_READY\\n'\n");await expect.poll(()=>terminal('replay',{sequence:'0'}).records.map((r:any)=>Buffer.from(r.base64,'base64').toString()).join('')).toContain('WS_READY\r\n');
 terminal('resize',{token,rows:37,columns:97});
 const csrf=(await (await page.request.get('http://127.0.0.1:18089/api/v1/session')).json()).csrf;
 const cookie=(await context.cookies()).map(c=>`${c.name}=${c.value}`).join('; ');
 const request=(sequence:string)=>({runId:run.id,sessionId:session.id,operation:'replay',params:{sequence},csrf});
 const rss=()=>Number(remote('pid=$(sudo systemctl show graphlab-m6-api --property=MainPID --value); sudo awk \'/VmRSS:/{print $2}\' /proc/$pid/status').trim());
 const before=rss(),slow=new Peer(),fast=new Peer();await slow.open(cookie);await fast.open(cookie);
 slow.socket.pause();
 input("printf 'BEGIN_DATA\\n'; i=0; while [ $i -lt 8 ]; do head -c 524288 /dev/zero | tr '\\000' X; i=$((i+1)); sleep 0.25; done; printf '\\nEND_DATA\\n'\n");
 const flood=(async()=>{for(let i=0;i<200;i++){slow.send(request('0'));await sleep(30);}})();
 let sequence='0',offset=0n;const records:any[]=[];let decoded='';const rssSamples:number[]=[];
 for(let i=0;i<300&&!decoded.includes('END_DATA\r\n');i++){
  const p=await fast.request(request(sequence));expect(p.status).toBe(202);expect(p.records.length).toBeLessThanOrEqual(128);let bytes=0;
  for(const r of p.records){expect(r.sequence).toBe(sequence);expect(r.offset).toBe(offset.toString());sequence=(BigInt(sequence)+1n).toString();const b=Buffer.from(r.base64,'base64');bytes+=b.length;if(r.type===1){offset+=BigInt(b.length);decoded+=b.toString();}records.push(r);}
  expect(bytes).toBeLessThanOrEqual(65536);expect(p.result.next).toBe(sequence);if(i%20===0)rssSamples.push(rss());await sleep(30);
 }
 expect(decoded).toContain('END_DATA\r\n');expect(decoded.split('BEGIN_DATA\r\n')[1].split('\r\nEND_DATA')[0]).toBe('X'.repeat(4194304));
 await flood;rssSamples.push(rss());expect(Math.max(...rssSamples)-before).toBeLessThan(32768);
 expect(slow.socket.readableLength).toBeLessThanOrEqual(131072);expect(slow.socket.destroyed).toBe(false);
 slow.close();fast.close();
 // Disconnect the fast viewer, reconnect at its exact next cursor, and resize
 // through the recorded PTY. A separate late peer replays independently from 0.
 const reconnect=new Peer();await reconnect.open(cookie);const continuation=await reconnect.request(request(sequence));expect(continuation.status).toBe(202);for(const r of continuation.records){records.push(r);sequence=(BigInt(r.sequence)+1n).toString();}reconnect.close();
 const late=new Peer();await late.open(cookie);let cursor='0';const replayed:any[]=[];
 while(BigInt(cursor)<BigInt(sequence)){const p=await late.request(request(cursor));expect(p.status).toBe(202);for(const r of p.records){if(BigInt(r.sequence)<BigInt(sequence))replayed.push(r);}expect(BigInt(p.result.next)).toBeGreaterThan(BigInt(cursor));cursor=p.result.next;await sleep(30);}
 late.close();expect(replayed).toEqual(records);expect(records.some(r=>r.type===2&&JSON.parse(Buffer.from(r.base64,'base64').toString()).rows===37)).toBe(true);
 terminal('close');const current=control('run',{id:run.id});const cleanup=control('operate',{runId:run.id,expectedRevision:current.revision,operation:'recover',idempotencyKey:'t12-cleanup-'+run.id});
 await expect.poll(()=>control('job',{id:cleanup.jobId}).state,{timeout:30000}).toBe('succeeded');
 writeFileSync(resolve(import.meta.dirname,'../../../build/m6-terminal-ws-proof.json'),JSON.stringify({runId:run.id,sessionId:session.id,outputBytes:offset.toString(),nextSequence:sequence,recordCount:records.length,pausedViewerRequests:200,apiRssBeforeKiB:before,apiRssSamplesKiB:rssSamples,replaySha256:createHash('sha256').update(JSON.stringify(records)).digest('hex'),cleanup},null,2));
});
