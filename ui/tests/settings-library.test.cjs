// Settings and list-state regressions. Use FSCS_PLAYWRIGHT to locate Playwright.
const {chromium}=require(process.env.FSCS_PLAYWRIGHT || 'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
const page=await browser.newPage({viewport:{width:800,height:950}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
await page.goto('file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface=main');await page.waitForFunction(()=>FSCApp.state.snapshot);
await page.locator('#mc-nav-settings').click();
for(const value of ['icons','labels','both']){
 await page.locator('#mc-setting-name-display [data-value='+value+']').click();
 await page.waitForFunction(v=>FSCApp.state.working.settings.ui.nameDisplay===v,value);
 assert.equal(await page.locator('#mc-setting-name-display [aria-pressed=true]').count(),1);
}
for(const value of ['light','dark']){
 await page.locator('#mc-setting-theme [data-value='+value+']').click();
 await page.waitForFunction(v=>document.getElementById('mc-sim').dataset.theme===v,value);
 assert.equal(await page.locator('#mc-setting-theme [aria-pressed=true]').count(),1);
}
// Leave values different from the saved defaults so Save is meaningfully enabled.
await page.locator('#mc-setting-name-display [data-value=labels]').click();
await page.locator('#mc-setting-theme [data-value=light]').click();
await page.locator('#mc-save').click();await page.waitForFunction(()=>!FSCApp.state.pendingCommit&&!FSCApp.state.inflight&&!FSCApp.state.queue.length);
await page.evaluate(()=>FSCApp.bridge.request('RequestSnapshot',{}));
assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.ui.nameDisplay),'labels');
assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.ui.theme),'light');
await page.locator('#mc-nav-cameras').click();
const sidebar=await page.locator('.mc-browser').boundingBox(),add=await page.locator('.mc-browser-bottom').boundingBox();
assert.ok(sidebar.y+sidebar.height-(add.y+add.height)<20,'Add camera stays at sidebar bottom');
const name=page.locator('.mc-camera-select .mc-entity-label').first();
assert.ok((await name.boundingBox()).width>=90,'camera label has usable width on a narrow editor');
await page.locator('#mc-toggle-categories').click();await page.locator('#mc-toggle-categories').click();
assert.equal(await page.locator('.mc-category-head[aria-expanded=true]').count(),0);
const profile=await page.evaluate(()=>JSON.parse(JSON.stringify(FSCApp.state.working.profile)));
const selected=await page.evaluate(()=>FSCApp.state.selection.cameraId);
const selectedGroup=profile.categories.find(g=>g.cameraIds.includes(selected));
const other=profile.categories.find(g=>g.id!==selectedGroup.id);
for(const group of [other,selectedGroup]){
 await page.locator('[data-drop-kind=category][data-drop-id="'+group.id+'"]').click({button:'right'});
 await page.locator('#mc-dialog-body').getByRole('button',{name:'Delete category',exact:true}).click();
 await page.locator('#mc-dialog-actions').getByRole('button',{name:'Delete',exact:true}).click();
 await page.waitForFunction(id=>!FSCApp.state.working.profile.categories.some(g=>g.id===id),group.id);
 await page.waitForTimeout(350);
 assert.equal(await page.locator('.mc-category-head[aria-expanded=true]').count(),0,'deletion preserves collapsed groups');
}
await page.screenshot({path:'build/review/seventh-library-collapsed.png'});
assert.deepEqual(errors,[]);console.log('Settings choices, save/snapshot, sidebar sizing and deletion collapse-state checks passed');
}finally{await browser.close()}})().catch(e=>{console.error(e);process.exitCode=1});
