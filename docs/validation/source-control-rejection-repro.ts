// Append temporarily to console/web/tests/parity.spec.ts; uses its existing setup helper.
test("verification: rejected source command becomes unrecoverable after refresh", async ({page}) => {
 await setup(page);
 await page.route("**/source-controls/query",r=>r.fulfill({json:{runState:"ready",instance:"instance-b",commands:[],capability:{apiVersion:"graphlab.source-control/v1",epoch:"a".repeat(32),sources:[{id:"alpha",state:"running",generatedDatagrams:"4"}]}}}));
 await page.route("**/source-controls/command",r=>r.fulfill({status:409,json:{error:{code:"source_epoch_changed"}}}));
 await page.getByText("Accessible inventory · all nodes and data edges").click();
 await page.getByRole("button",{name:"a · docker · runtime ready",exact:true}).click();
 const panel=page.getByRole("region",{name:"Source controls"});
 await panel.getByRole("button",{name:"Pause source alpha",exact:true}).click();
 await expect(panel.getByRole("alert")).toContainText("source epoch changed");
 await expect(panel.getByRole("alert")).toHaveCount(0,{timeout:4000});
 await expect(panel.getByRole("button",{name:"Pause source alpha",exact:true})).toBeDisabled();
 await expect(panel.getByRole("button",{name:"Resume source alpha",exact:true})).toBeDisabled();
 await expect(panel.getByRole("button",{name:"Retry same request",exact:true})).toHaveCount(0);
 await expect(panel.getByRole("status")).toContainText("requested");
 console.log("REPRODUCED: rejected request remains requested; all source buttons disabled and retry/error disappear after successful polling");
});
