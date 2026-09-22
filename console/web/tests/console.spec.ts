import {test,expect} from '@playwright/test';
import {spawn,execFileSync,type ChildProcess} from 'node:child_process';
import {mkdtempSync,rmSync,existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve,join} from 'node:path';
import {graph,type Inventory} from '../src/graph';
let directory:string,password:string,agent:ChildProcess,api:ChildProcess;
const root=resolve(import.meta.dirname,'../../..');
async function stop(child:ChildProcess){if(child?.exitCode===null){await new Promise<void>(done=>{child.once('exit',()=>done());child.kill('SIGTERM');});}}
test.beforeAll(async()=>{
 directory=mkdtempSync(join(tmpdir(),'graphlab-browser-'));
 password=execFileSync(join(root,'build/dev/lab-api'),['init-auth',join(directory,'auth.json')],{encoding:'utf8'}).trim();
 agent=spawn(join(root,'build/dev/lab-agent'),['--socket',join(directory,'agent.sock'),'--topologies',join(root,'topologies'),'--lock',join(root,'topologies/artifacts.lock.json')],{stdio:'ignore'});
 for(let i=0;i<100&&!existsSync(join(directory,'agent.sock'));i++)await new Promise(r=>setTimeout(r,20));
 api=spawn(join(root,'build/dev/lab-api'),['--socket',join(directory,'agent.sock'),'--auth',join(directory,'auth.json'),'--assets',join(root,'console/web/dist'),'--port','18088'],{stdio:'ignore'});
 for(let i=0;i<100;i++){try{await fetch('http://127.0.0.1:18088/');break;}catch{await new Promise(r=>setTimeout(r,20));}}
});
test.afterAll(async()=>{await stop(api);await stop(agent);if(directory)rmSync(directory,{recursive:true,force:true});});
// Respect the real service's global login rate limit between independent sessions.
test.beforeEach(async()=>{await new Promise(resolve=>setTimeout(resolve,550));});
test('all graph shapes, parallel edges, management, inspector, reload and logout',async({page})=>{
 const errors:string[]=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('/');await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await expect(page.getByRole('navigation',{name:'Topologies'})).toBeVisible();
 for(const [name,nodes,edges] of [['chain',3,2],['star',4,3],['ring',3,3],['mesh',4,6],['disconnected',3,1],['isolated',3,0],['parallel',3,4],['triangle',6,6]] as const){
   await page.getByRole('navigation').getByRole('button',{name:new RegExp(`^${name}`)}).click();
   await expect(page.getByRole('heading',{name,exact:true})).toBeVisible();
   await expect(page.locator('.react-flow__node')).toHaveCount(nodes);
   await expect(page.locator('.react-flow__edge')).toHaveCount(edges);
   if(name==='parallel'){
     const paths=await page.locator('.react-flow__edge-path').evaluateAll(elements=>elements.map(e=>e.getAttribute('d')));
     expect(new Set(paths).size).toBe(edges);
   }
 }
 await expect(page.getByText('○ Runtime mappings: unknown')).toBeVisible();
 await page.getByLabel('Management layer').check();
 await expect(page.locator('.react-flow__node')).toHaveCount(7);await expect(page.locator('.react-flow__edge')).toHaveCount(9);
 await page.getByText('Accessible inventory · all nodes and data edges').click();
 await page.getByRole('button',{name:'a · docker · runtime unknown',exact:true}).click();
 await expect(page.getByRole('heading',{name:'Resource details'})).toBeVisible();
 await expect(page.locator('.inspector pre')).toContainText('"identity": null');
 await page.getByRole('button',{name:'Clear selection'}).click();
 await page.screenshot({path:join(root,'docs/validation/m1-console.png'),fullPage:true});
 await page.reload();await expect(page.getByRole('navigation',{name:'Topologies'})).toBeVisible();
 await page.getByRole('button',{name:'Sign out'}).click();await expect(page.getByLabel('Operator credential')).toBeVisible();
 expect(errors).toEqual([]);
});
test('layout supports variable sizes, reversed parallel edges and self-links',()=>{
 for(const count of [1,2,7,31,100]){
 const inventory={nodes:Array.from({length:count},(_,i)=>({id:`s${i}`,kind:'ovs-switch',runtime:{state:'unknown'},failureDomain:'host/shared-ovs'})),edges:[],management:{networks:{}},managementAttachments:[]} as unknown as Inventory;
 expect(graph(inventory,false).nodes).toHaveLength(count);
 }
 const fixture={nodes:[{id:'a',kind:'ovs-switch',runtime:{state:'unknown'}},{id:'b',kind:'ovs-switch',runtime:{state:'unknown'}}],edges:[{id:'one',endpoints:['a:p1','b:p1']},{id:'two',endpoints:['b:p2','a:p2']},{id:'self',endpoints:['a:p3','a:p4']}],management:{networks:{}},managementAttachments:[]} as unknown as Inventory;
 const view=graph(fixture,false);expect(view.edges).toHaveLength(3);expect(view.edges[2].source).toBe(view.edges[2].target);
 expect(view.edges[0].data?.offset).not.toBe(view.edges[1].data?.offset);
});
test('invalidated session returns to login on refresh',async({page,context})=>{
 await page.goto('/');await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await expect(page.getByRole('navigation',{name:'Topologies'})).toBeVisible();await context.clearCookies();
 await page.getByRole('button',{name:'Refresh',exact:true}).click();await expect(page.getByLabel('Operator credential')).toBeVisible();
});
test('agent loss clears the graph and exposes an unavailable diagnostic',async({page})=>{
 await page.goto('/');await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await expect(page.locator('.react-flow__node').first()).toBeVisible();await stop(agent);await page.getByRole('button',{name:'Refresh',exact:true}).click();
 await expect(page.getByRole('alert')).toContainText('Agent unavailable');await expect(page.locator('.react-flow__node')).toHaveCount(0);
});
