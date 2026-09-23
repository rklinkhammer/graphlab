import {test,expect} from '@playwright/test';
import {spawn,type ChildProcess} from 'node:child_process';
let server:ChildProcess;
test.beforeAll(async()=>{
 server=spawn(process.execPath,['node_modules/vite/bin/vite.js','--host','127.0.0.1','--port','18090','--strictPort'],{stdio:'ignore'});
 for(let i=0;i<100;i++){try{await fetch('http://127.0.0.1:18090/tests/terminal-harness.html');return;}catch{await new Promise(r=>setTimeout(r,30));}}
 throw new Error('terminal fixture server unavailable');
});
test.afterAll(async()=>{if(server?.exitCode===null)await new Promise<void>(done=>{server.once('exit',()=>done());server.kill();});});
test('hostile terminal bytes remain terminal data',async({page})=>{
 const errors:string[]=[];const requests:string[]=[];
 page.on('pageerror',e=>errors.push(e.message));
 page.on('request',r=>{if(r.url().includes('attacker.invalid'))requests.push(r.url());});
 await page.routeWebSocket('**/api/v1/terminal',socket=>{
  let sent=false;
  socket.onMessage(()=>{
   if(sent)return;sent=true;
   const corpus=[
    '<img src="https://attacker.invalid/pixel" onerror="window.pwned=1">',
    '<script>window.pwned=1</script>',
    '\x1b]52;c;c2VjcmV0\x07',
    '\x1b]8;;javascript:window.pwned=1\x1b\\click\x1b]8;;\x1b\\',
    '\x1b]0;<img onerror="window.pwned=1">\x07',
    '\x1bP$q'+ 'x'.repeat(65536)+'\x1b\\',
    '\x1b[9999999999999999999999999999999999999A\x00\xff',
   ];
   let seed=0x6c6162;
   for(let i=0;i<256;i++){
    let bytes='';for(let j=0;j<128;j++){seed=(Math.imul(seed,1664525)+1013904223)>>>0;bytes+=String.fromCharCode(seed&255);}
    corpus.push(['\x1b[','\x1b]','\x1bP','\x1b_'][i%4]+bytes+'\x18\x1b\\');
   }
   corpus.push('\x18\x1b\\\x1b[0m\r\nM6_CORPUS_COMPLETE');
   for(const payload of corpus){const bytes=Buffer.from(payload);const frame=Buffer.alloc(24+bytes.length);frame.writeUInt32BE(1,16);frame.writeUInt32BE(bytes.length,20);bytes.copy(frame,24);socket.send(frame);}
   socket.send(JSON.stringify({status:200,result:{next:'7'}}));
  });
 });
 await page.goto('http://127.0.0.1:18090/tests/terminal-harness.html');
 await page.getByRole('combobox',{name:'Console',exact:true}).selectOption('session');
 await expect(page.locator('.xterm-screen')).toBeVisible();
 await expect.poll(()=>page.locator('.xterm-rows').innerText()).toContain('M6_CORPUS_COMPLETE');
 expect(await page.evaluate(()=>('pwned' in window))).toBe(false);
 expect(await page.title()).toBe('Terminal qualification fixture');
 expect(requests).toEqual([]);expect(errors).toEqual([]);
 await expect(page.getByRole('button',{name:'Reconnect and replay'})).toBeEnabled();
});
