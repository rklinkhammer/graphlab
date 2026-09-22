import {test,expect} from '@playwright/test';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
test('M5 directional telemetry, fault preview, expiry and timeline',async({page})=>{
 test.skip(!process.env.GRAPHLAB_M5_LIVE,'Requires dedicated Linux M5 services');test.setTimeout(120000);
 const root=resolve(import.meta.dirname,'../../..');await page.goto('http://127.0.0.1:18089/');await page.getByLabel('Operator credential').fill(readFileSync(resolve(root,'build/m5-live-password'),'utf8').trim());await page.getByRole('button',{name:'Sign in',exact:true}).click();
 await page.getByLabel('I accept running without capture coverage.').check();await page.getByRole('button',{name:'Start selected topology'}).click();await expect(page.getByRole('status').filter({hasText:'start: succeeded'})).toBeVisible({timeout:60000});
 const panel=page.getByRole('region',{name:'Telemetry and faults'});
 await expect(panel.getByRole('button',{name:'a-s',exact:true})).toBeVisible({timeout:20000});await panel.getByRole('button',{name:'a-s',exact:true}).click();
 await panel.getByLabel('Duration seconds').fill('3');await panel.getByRole('button',{name:'Preview fault placement'}).click();await expect(panel.getByRole('button',{name:'Apply directional fault'})).toBeVisible();await panel.getByRole('button',{name:'Apply directional fault'}).click();
 await expect(panel.getByRole('status')).toContainText('succeeded',{timeout:15000});await expect(panel.getByText(/a-s a-to-b.*removed/)).toBeVisible({timeout:15000});
 await panel.getByText(/Correlated timeline/).click();await expect(panel.getByText(/fault-expired/, {exact:false}).first()).toBeVisible();await expect(panel.getByRole('img')).toBeVisible();
 await page.screenshot({path:resolve(root,'docs/validation/m5-console.png'),fullPage:true});
 await page.getByRole('button',{name:'Destroy run',exact:true}).click();await expect(page.getByRole('status').filter({hasText:'destroy: succeeded'})).toBeVisible({timeout:30000});
});
