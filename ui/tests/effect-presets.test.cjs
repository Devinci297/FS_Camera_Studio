const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
const KEYS=['intensity','turbulence','groundRoll','touchdown','acceleration','engine'];
const PRESETS={
 subtle:{enabled:true,intensity:.18,turbulence:.25,groundRoll:.18,touchdown:.25,acceleration:.18,engine:.1},
 realistic:{enabled:true,intensity:.35,turbulence:.5,groundRoll:.35,touchdown:.5,acceleration:.35,engine:.2},
 action:{enabled:true,intensity:.63,turbulence:.9,groundRoll:.63,touchdown:.9,acceleration:.63,engine:.36},
 off:{enabled:false,intensity:.35,turbulence:.5,groundRoll:.35,touchdown:.5,acceleration:.35,engine:.2}
};
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const select=async mode=>{await page.evaluate(mode=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode===mode);s.selection.cameraId=c.id;window.effectCameraId=c.id;FSCDemo.setOwnership('Observing');},mode);await page.locator('#mc-tab-motion').click();};
  const effects=()=>page.evaluate(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId).effects);
  const preset=page.locator('#mc-effects-preset');

  await select('aircraftCockpit');
  assert.equal(await preset.inputValue(),'off');assert.equal(await preset.isDisabled(),false);
  for(const name of ['realistic','subtle','action','off']){
   await preset.selectOption(name);
   const fx=await effects();assert.equal(fx.enabled,PRESETS[name].enabled,name);
   for(const key of KEYS)assert.equal(fx[key],PRESETS[name][key],name+' '+key);
   assert.equal(await preset.inputValue(),name);
   assert.equal(await page.locator('#mc-effects-enabled').isChecked(),PRESETS[name].enabled);
   assert.equal(await page.locator('[data-effect="turbulence"]').inputValue(),String(Math.round(PRESETS[name].turbulence*100)));
  }
  // Enabling with default values is the Realistic preset.
  await page.locator('#mc-effects-enabled').check();assert.equal(await preset.inputValue(),'realistic');
  // A manual slider edit keeps the other values and shows Custom; restoring it matches the preset again.
  await preset.selectOption('action');
  const slider=page.locator('[data-effect="groundRoll"]');await slider.fill('40');await slider.dispatchEvent('input');
  await page.waitForFunction(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId).effects.groundRoll===.4);
  assert.equal(await preset.inputValue(),'custom');
  const custom=await effects();assert.equal(custom.enabled,true);assert.equal(custom.intensity,PRESETS.action.intensity);
  await slider.fill('63');await slider.dispatchEvent('input');assert.equal(await preset.inputValue(),'action');
  await slider.fill('64');await slider.dispatchEvent('input');assert.equal(await preset.inputValue(),'custom');
  assert.equal(await page.locator('#mc-effects-preset option[value="custom"]').evaluate(o=>o.disabled),true);

  // Exterior and orbit views take effects at reduced strength; world-fixed views stay locked.
  for(const mode of ['aircraftExterior','orbit']){
   await select(mode);assert.equal(await preset.isDisabled(),false,mode);assert.equal(await slider.isDisabled(),false,mode);
   await preset.selectOption('subtle');assert.equal((await effects()).acceleration,PRESETS.subtle.acceleration);
   assert.match(await page.locator('#mc-effects-status').innerText(),/half strength/);
  }
  for(const mode of ['airport','flyby']){
   if(!await page.evaluate(mode=>FSCApp.state.working.profile.cameras.some(c=>c.mode===mode),mode))continue;
   await select(mode);assert.equal(await preset.isDisabled(),true,mode);assert.equal(await slider.isDisabled(),true,mode);
   assert.match(await page.locator('#mc-effects-status').innerText(),/cockpit, exterior and orbit/);
  }
  assert.doesNotMatch(await page.locator('#mc-detail-effects').innerText(),/Cockpit views only/);

  const persisted=await page.evaluate(async()=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('RevertChanges',{});return FSCApp.state.working.profile.cameras.find(c=>c.mode==='orbit').effects;});
  for(const key of KEYS)assert.equal(persisted[key],PRESETS.subtle[key],key);
  assert.deepEqual(errors,[]);console.log('Effect presets apply all values, manual edits show Custom, and exterior/orbit eligibility passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
