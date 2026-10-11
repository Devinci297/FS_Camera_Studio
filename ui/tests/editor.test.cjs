const {chromium}=require(process.env.FSCS_PLAYWRIGHT || 'playwright');const assert=require('node:assert/strict');const path=require('node:path');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
const page=await browser.newPage({viewport:{width:1100,height:1000}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');await page.waitForFunction(()=>FSCApp.state.snapshot);
for(const size of [{width:1100,height:1000},{width:800,height:950}]){await page.setViewportSize(size);await page.waitForTimeout(60);const dimensions=await page.locator('#mc-inspector').evaluate(e=>({width:e.clientWidth,scroll:e.scrollWidth}));assert.ok(dimensions.scroll<=dimensions.width+2,JSON.stringify({size,dimensions}));const diagram=await page.locator('.mc-axis-illustration').first().boundingBox();assert.ok(diagram.height>=70);await page.screenshot({path:'build/review/pose-'+size.width+'.png'});}
await page.setViewportSize({width:1100,height:1000});
await page.locator('#mc-camera-name').fill('Pilot custom');await page.locator('#mc-camera-name').press('Enter');await page.waitForFunction(()=>FSCApp.state.working.profile.cameras[0].name==='Pilot custom');
const original=await page.evaluate(()=>JSON.parse(JSON.stringify(FSCApp.state.working.profile.cameras[0])));
await page.locator('#mc-pose-x').fill('.123');await page.locator('#mc-pose-x').press('Tab');await page.locator('[data-reset-axis="x"]').click();assert.equal(Number(await page.locator('#mc-pose-x').inputValue()),0);
const pose=await page.evaluate(()=>FSCApp.state.working.profile.cameras[0].positionMetersFRD);assert.equal(pose[0],original.positionMetersFRD[0]);assert.equal(pose[2],original.positionMetersFRD[2]);
assert.equal(await page.locator('[data-reset-axis]').count(),6);
assert.equal(await page.locator('[data-pose-slider]').count(),0);
assert.equal(await page.locator('[data-angle-control]').count(),3);
for(const axis of ['y','z']){const plus=await page.locator('[data-axis='+axis+'] .mc-direction-plus').boundingBox(),minus=await page.locator('[data-axis='+axis+'] .mc-direction-minus').boundingBox();assert.ok(minus.x+minus.width<=plus.x+1 || plus.y+plus.height<=minus.y+1);}
const beforeX=Number(await page.locator('#mc-pose-x').inputValue());await page.locator('[data-axis=x] .mc-direction-plus').click();assert.ok(Number(await page.locator('#mc-pose-x').inputValue())>beforeX);
const yaw=page.locator('[data-angle-control=heading]');await yaw.press('Home');await yaw.press('ArrowRight');assert.equal(Number(await yaw.getAttribute('aria-valuenow')),1);
const rect=await yaw.boundingBox(),scale=Math.min(rect.width/240,rect.height/150),ox=rect.x+(rect.width-240*scale)/2,oy=rect.y+(rect.height-150*scale)/2;
await page.mouse.move(ox+120*scale,oy+23*scale);await page.mouse.down();await page.mouse.move(ox+172*scale,oy+75*scale,{steps:6});await page.mouse.up();assert.ok(Math.abs(Number(await yaw.getAttribute('aria-valuenow'))-90)<=1);
assert.ok((await yaw.locator('.mc-angle-zone').getAttribute('d')).includes('A52 52'));
await page.locator('#mc-pose-pitch').fill('25');await page.locator('#mc-pose-bank').fill('30');await page.locator('#mc-pose-bank').press('Tab');await page.screenshot({path:'build/review/directional-controls.png'});

await page.locator('[data-reset-axis=heading]').click();assert.equal(Number(await yaw.getAttribute('aria-valuenow')),0);

await page.locator('#mc-camera-binding').click();await page.locator('#mc-assign-capture').press('Control+Alt+2');assert.ok((await page.locator('#mc-dialog-error').textContent()).includes('PFD'));
await page.getByRole('button',{name:'Replace existing assignment',exact:true}).click();await page.waitForFunction(()=>FSCApp.state.working.bindings.bindings.some(b=>b.viewId===1&&b.input.definition==='VK_LCONTROL+VK_LMENU+2'));
assert.equal(await page.evaluate(()=>FSCApp.state.working.bindings.bindings.filter(b=>b.input.definition==='VK_LCONTROL+VK_LMENU+2').length),1);
await page.locator('#mc-camera-binding').click();await page.getByRole('button',{name:'Clear assignment',exact:true}).click();await page.waitForFunction(()=>!FSCApp.state.working.bindings.bindings.some(b=>b.action==='selectView'&&b.viewId===1));
await page.locator('#mc-tab-motion').click();await page.locator('#mc-effects-enabled').check();await page.locator('[data-effect="groundRoll"]').fill('23');await page.locator('[data-effect="groundRoll"]').dispatchEvent('input');await page.waitForFunction(()=>FSCApp.state.working.profile.cameras[0].effects.groundRoll===.23);
await page.screenshot({path:'build/review/motion-effects.png'});
await page.locator('#mc-tab-behavior').click();
assert.equal(await page.locator('#mc-mouse-release [data-value=retain]').getAttribute('aria-pressed'),'true');
await page.locator('#mc-mouse-release [data-value=reset]').click();
await page.waitForFunction(()=>FSCApp.state.working.profile.cameras[0].mouseLookOnRelease==='reset');
assert.equal(await page.locator('#mc-mouse-release [data-value=reset]').getAttribute('aria-pressed'),'true');
await page.screenshot({path:'build/review/mouse-release-behavior.png'});
await page.locator('#mc-save').click();await page.waitForFunction(()=>!FSCApp.state.pendingCommit&&!FSCApp.state.inflight&&!FSCApp.state.queue.length&&!FSCApp.state.working.dirty.profile);
const saved=await page.evaluate(async()=>JSON.parse((await FSCApp.bridge.request('ExportProfile',{profileId:FSCApp.state.working.profile.profileId,working:false})).result.code));assert.equal(saved.cameras[0].effects.enabled,true);assert.equal(saved.cameras[0].effects.groundRoll,.23);assert.equal(saved.cameras[0].name,'Pilot custom');assert.equal(saved.cameras[0].mouseLookOnRelease,'reset');
assert.deepEqual(errors,[]);console.log('Editor horizontal bounds, per-axis reset, inline rename, conflict replacement/clear, and saved effects passed');
}finally{await browser.close()}})().catch(e=>{console.error(e);process.exit(1)});
