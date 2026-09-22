import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
test('M2 browser admits, quiesces, resumes and destroys a real Linux run',async({page})=>{
 test.skip(!process.env.GRAPHLAB_M2_LIVE,'Explicit Linux runtime fixture and tunnel required');
 test.setTimeout(120000);
 const root=resolve(import.meta.dirname,'../../..');
 const password=readFileSync(resolve(root,'build/m2-live-password'),'utf8').trim();
 await page.goto('http://127.0.0.1:18089/');await page.getByLabel('Operator credential').fill(password);await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await expect(page.getByText('M2 development execution',{exact:true})).toBeVisible();
 await page.getByLabel('I accept running without capture coverage.').check();await page.getByRole('button',{name:'Start selected topology'}).click();
 await expect(page.getByRole('status')).toContainText('start: succeeded',{timeout:60000});
 await page.getByRole('button',{name:'Refresh',exact:true}).click();await expect(page.getByText('○ Runtime mappings: identity snapshots')).toBeVisible();
 await page.screenshot({path:resolve(root,'docs/validation/m2-console.png'),fullPage:true});
 await page.getByRole('button',{name:'Quiesce',exact:true}).click();await expect(page.getByRole('status')).toContainText('stop: succeeded',{timeout:30000});
 await page.getByRole('button',{name:'Resume',exact:true}).click();await expect(page.getByRole('status')).toContainText('resume: succeeded',{timeout:30000});
 await page.getByRole('button',{name:'Destroy run',exact:true}).click();await expect(page.getByRole('status')).toContainText('destroy: succeeded',{timeout:30000});
 await expect(page.getByRole('button',{name:'Start selected topology'})).toBeVisible();
});
