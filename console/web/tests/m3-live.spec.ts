import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
test('M3 browser captures, closes and downloads verified artifacts',async({page})=>{
 test.skip(!process.env.GRAPHLAB_M3_LIVE,'Explicit Linux M3 services and tunnel required');test.setTimeout(120000);
 const root=resolve(import.meta.dirname,'../../..');
 await page.goto('http://127.0.0.1:18089/');await page.getByLabel('Operator credential').fill(readFileSync(resolve(root,'build/m3-live-password'),'utf8').trim());await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await expect(page.getByText('M3 capture-first execution',{exact:true})).toBeVisible();
 await page.getByRole('button',{name:'Start selected topology'}).click();
 await expect(page.getByRole('status')).toContainText('start: succeeded',{timeout:60000});
 await expect(page.getByText(/coverage: recording/)).toBeVisible();
 await page.getByRole('button',{name:'Quiesce',exact:true}).click();await expect(page.getByRole('status')).toContainText('stop: succeeded',{timeout:30000});
 const download=page.waitForEvent('download');await page.getByRole('button',{name:/Download PCAPNG/}).first().click();const file=await download;await file.saveAs(resolve(root,'build/m3-browser-download.pcapng'));
 await page.getByRole('button',{name:'Refresh',exact:true}).click();await expect(page.getByText('○ Runtime mappings: identity snapshots')).toBeVisible();
 await page.screenshot({path:resolve(root,'docs/validation/m3-console.png'),fullPage:true});
 await page.getByRole('button',{name:'Resume',exact:true}).click();await expect(page.getByRole('status')).toContainText('resume: succeeded',{timeout:30000});
 await page.getByRole('button',{name:'Destroy run',exact:true}).click();await expect(page.getByRole('status')).toContainText('destroy: succeeded',{timeout:30000});
});
