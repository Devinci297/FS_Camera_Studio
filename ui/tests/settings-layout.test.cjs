const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1450,height:1100}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.locator('#mc-nav-settings').click();
  const sections={appearance:'mc-setting-main-opacity',movement:'mc-setting-move-speed',tracking:'mc-setting-global-head-tracking',airport:'mc-setting-airport-autozoom',updates:'mc-update-check'};
  for(const [key,id] of Object.entries(sections)){
   await page.locator('#mc-settings-link-'+key).click();
   assert.equal(await page.locator('#'+id).isVisible(),true,key+' shows its controls');
   assert.equal(await page.locator('.mc-settings-pane:visible').count(),1,'Only the chosen Settings section is visible');
   assert.equal(await page.locator('#mc-settings-link-'+key).getAttribute('aria-current'),'page');
  }
  await page.locator('#mc-settings-link-movement').click();
  await page.locator('#mc-setting-move-speed').evaluate(input=>{input.value='1.5';input.dispatchEvent(new Event('change',{bubbles:true}));});
  await page.locator('#mc-settings-link-airport').click();await page.locator('#mc-setting-airport-autozoom').check();
  await page.locator('#mc-setting-airport-height-gates').evaluate(input=>{input.value='9';input.dispatchEvent(new Event('change',{bubbles:true}));});
  await page.locator('#mc-settings-link-appearance').click();
  await page.locator('#mc-setting-main-opacity').evaluate(input=>{input.value='75';input.dispatchEvent(new Event('input',{bubbles:true}));});
  for(const width of [1450,560]){
   await page.setViewportSize({width,height:1100});
   for(const section of ['appearance','airport']){
    await page.locator('#mc-settings-link-'+section).click();
    const layout=await page.locator('#mc-page-settings').evaluate(page=>{
     const bounds=page.getBoundingClientRect(),visible=Array.from(page.querySelectorAll('input,select,button')).filter(el=>el.getClientRects().length);
     return {fits:visible.every(el=>{const r=el.getBoundingClientRect();return r.left>=bounds.left-1&&r.right<=bounds.right+1;}),overflow:page.scrollWidth>page.clientWidth+1};
    });
    assert.equal(layout.fits,true,section+' controls fit at '+width);assert.equal(layout.overflow,false,section+' has no horizontal scroll at '+width);
    await page.locator('#mc-page-settings').screenshot({path:'build/review/ui-settings-'+section+'-'+width+'.png'});
   }
  }
  await page.setViewportSize({width:1450,height:1100});
  await page.locator('#mc-settings-link-appearance').click();await page.locator('#mc-app-version').click();
  await page.locator('#mc-update-check').waitFor({state:'visible'});
  assert.equal(await page.locator('#mc-update-check').isVisible(),true,'Version badge opens the Updates section');
  await page.waitForTimeout(600);
  const expected=await page.evaluate(()=>{const s=FSCApp.state.working.settings;return [s.host.moveSpeedMetersPerSec,s.host.airportAutoZoomEnabled,s.host.airportCategoryHeights.gates,s.ui.mainOpacity];});
  assert.deepEqual(expected,[1.5,true,9,.75]);
  await page.evaluate(async()=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,300));await FSCApp.bridge.request('RevertChanges',{});});
  await page.waitForTimeout(400);
  assert.deepEqual(await page.evaluate(()=>{const s=FSCApp.state.working.settings;return [s.host.moveSpeedMetersPerSec,s.host.airportAutoZoomEnabled,s.host.airportCategoryHeights.gates,s.ui.mainOpacity];}),expected,'Settings retain listeners and persist after reorganizing');
  assert.deepEqual(errors,[]);console.log('Settings section navigation, update badge, responsive controls and persistence passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
