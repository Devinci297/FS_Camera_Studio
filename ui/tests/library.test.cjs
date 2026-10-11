// Run from repository root; FSCS_PLAYWRIGHT may specify the Playwright module path.
const {chromium}=require(process.env.FSCS_PLAYWRIGHT || 'playwright');
const assert=require('node:assert/strict');const path=require('node:path');
(async()=>{
const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
try {
const page=await browser.newPage({viewport:{width:1100,height:1000}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
await page.goto('file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface=main');await page.waitForFunction(()=>window.FSCApp.state.snapshot);
const original=await page.evaluate(()=>JSON.parse(JSON.stringify(window.FSCApp.state.working.profile)));
const camera=original.cameras[0], first=original.categories[0], second=original.categories[1];
await page.locator('#mc-camera-icon').click();await page.locator('[data-icon-choice="view-wing"]').click();
await page.waitForFunction(id=>window.FSCApp.state.working.profile.cameras.find(c=>c.id===id).icon==='view-wing',camera.id);
assert.equal(await page.locator('[data-drop-id="'+camera.id+'"] .mc-entity-name svg').getAttribute('data-icon'),'view-wing');
const category=page.locator('[data-drop-kind="category"][data-drop-id="'+first.id+'"]');
await category.click({button:'right'});await page.locator('#mc-dialog-body').getByRole('button',{name:'Choose icon',exact:true}).click();await page.locator('[data-icon-choice="map"]').click();
await page.waitForFunction(id=>window.FSCApp.state.working.profile.categories.find(c=>c.id===id).icon==='map',first.id);
// The simulator may omit contextmenu: right-button mouseup must still open the menu.
await page.waitForTimeout(310);
await category.dispatchEvent('mouseup',{button:2,which:3,bubbles:true});
await page.locator('[data-color-choice="#75baff"]').click();
assert.equal(await page.locator('[data-drop-id="'+camera.id+'"] .mc-entity-name').evaluate(e=>getComputedStyle(e).color),'rgb(117, 186, 255)');
await page.locator('#mc-camera-color').click();await page.locator('[data-color-choice="#ffd27a"]').click();
assert.equal(await page.locator('[data-drop-id="'+camera.id+'"] .mc-entity-name').evaluate(e=>getComputedStyle(e).color),'rgb(255, 210, 122)');
await page.locator('#mc-category-default').click();assert.equal(await page.locator('#mc-category-default').getAttribute('aria-pressed'),'true');
// Real mouse grip drag, with no HTML dataTransfer, exercises the Coherent fallback.
const grip=page.locator('.mc-camera-row').filter({has:page.locator('[data-drop-id="'+camera.id+'"]')}).locator('.mc-reorder-grip');
const source=await grip.boundingBox();const target=await page.locator('[data-drop-id="'+second.id+'"]').boundingBox();
await page.mouse.move(source.x+source.width/2,source.y+source.height/2);await page.mouse.down();await page.mouse.move(target.x+target.width/2,target.y+target.height/2,{steps:8});await page.mouse.up();
await page.waitForFunction(({id,categoryId})=>window.FSCApp.state.working.profile.cameras.find(c=>c.id===id).categoryId===categoryId,{id:camera.id,categoryId:second.id});
assert.equal(await page.evaluate(id=>window.FSCApp.state.working.profile.categories.flatMap(g=>g.cameraIds).filter(x=>x===id).length,camera.id),1);
// Native drag reorder of categories.
const from=page.locator('[data-drop-id="'+second.id+'"]'),to=page.locator('[data-drop-id="'+first.id+'"]');
await from.dragTo(to,{targetPosition:{x:20,y:2}});
await page.waitForFunction(id=>window.FSCApp.state.working.profile.categories[0].id===id,second.id);
await page.locator('#mc-save').click();await page.waitForFunction(()=>!window.FSCApp.state.pendingCommit && !window.FSCApp.state.inflight && !window.FSCApp.state.queue.length && !window.FSCApp.state.working.dirty.profile);
const exported=await page.evaluate(async()=>{const a=await window.FSCApp.bridge.request('ExportProfile',{profileId:window.FSCApp.state.working.profile.profileId,working:false});return JSON.parse(a.result.code)});
assert.equal(exported.cameras.find(c=>c.id===camera.id).icon,'view-wing');assert.equal(exported.categories.find(c=>c.id===first.id).icon,'map');assert.equal(exported.categories[0].id,second.id);assert.equal(exported.categories.find(g=>g.id===first.id).color,'#75baff');assert.equal(exported.cameras.find(c=>c.id===camera.id).color,'#ffd27a');assert.equal(exported.categories.find(g=>g.id===first.id).defaultCameraId,undefined);
await page.goto('file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface=mini');await page.waitForFunction(()=>window.FSCApp.state.snapshot);
await page.evaluate(()=>document.dispatchEvent(new MouseEvent('pointermove',{clientX:5,clientY:995,bubbles:true})));
await page.waitForFunction(()=>document.getElementById('mc-sim').dataset.miniOpen==='true');
// A category selection starts its configured default, even when it is not first in the list.
const targetDefault=await page.evaluate(()=>{const g=FSCApp.state.working.profile.categories[0];g.defaultCameraId=g.cameraIds[1];return {id:g.id,cameraId:g.defaultCameraId};});
await page.locator('[data-focus-key="category:'+targetDefault.id+'"]').click();
await page.waitForFunction(id=>FSCApp.state.active.cameraId===id,targetDefault.cameraId);
// Native mouse input does not capture the scene through an HTML overlay.
await page.locator('#mc-mouse-look').click();
assert.equal(await page.locator('#mc-mouse-look-area').isVisible(),false);
assert.equal(await page.evaluate(()=>FSCApp.state.active.cameraId),targetDefault.cameraId);
assert.deepEqual(errors,[]);console.log('Icon persistence, camera/category reordering, and retargeted hover regressions passed');
} finally {await browser.close()}
})().catch(e=>{console.error(e);process.exitCode=1});
