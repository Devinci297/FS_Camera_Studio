const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Head-movement effects: cockpit-only sliders, part of every preset, defaults for profiles saved without them.
const BASE=['intensity','turbulence','groundRoll','touchdown','acceleration','engine'],HEAD=['headLag','turnLean','taxiBob','idleSway'];
const PRESETS={
 subtle:{enabled:true,intensity:.18,turbulence:.25,groundRoll:.18,touchdown:.25,acceleration:.18,engine:.1,headLag:.25,turnLean:.25,taxiBob:.25,idleSway:0},
 realistic:{enabled:true,intensity:.35,turbulence:.5,groundRoll:.35,touchdown:.5,acceleration:.35,engine:.2,headLag:.5,turnLean:.5,taxiBob:.5,idleSway:0},
 action:{enabled:true,intensity:.63,turbulence:.9,groundRoll:.63,touchdown:.9,acceleration:.63,engine:.36,headLag:.9,turnLean:.9,taxiBob:.9,idleSway:0},
 off:{enabled:false,intensity:.35,turbulence:.5,groundRoll:.35,touchdown:.5,acceleration:.35,engine:.2,headLag:.5,turnLean:.5,taxiBob:.5,idleSway:0}
};
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const select=async mode=>{await page.evaluate(mode=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode===mode);s.selection.cameraId=c.id;window.effectCameraId=c.id;FSCDemo.setOwnership('Observing');},mode);await page.locator('#mc-tab-motion').click();};
  const effects=()=>page.evaluate(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId).effects);
  const preset=page.locator('#mc-effects-preset'),group=page.locator('#mc-effects-head');
  const slider=key=>page.locator(`[data-effect="${key}"]`);

  await select('aircraftCockpit');
  assert.equal(await group.isVisible(),true);
  assert.match(await group.innerText(),/Head movement/);
  for(const key of HEAD){assert.equal(await slider(key).isVisible(),true,key);assert.equal(await slider(key).isDisabled(),false,key);}
  // Each preset is complete: it sets the head strengths along with the original six.
  for(const name of ['realistic','subtle','action','off']){
   await preset.selectOption(name);
   const fx=await effects();
   for(const key of BASE.concat(HEAD))assert.equal(fx[key],PRESETS[name][key],name+' '+key);
   assert.equal(await preset.inputValue(),name);
   for(const key of HEAD)assert.equal(await slider(key).inputValue(),String(Math.round(PRESETS[name][key]*100)),name+' '+key);
  }
  // Defaults are the Realistic preset; idle sway is optional and off.
  await page.locator('#mc-effects-enabled').check();assert.equal(await preset.inputValue(),'realistic');
  assert.equal(await page.locator('#mc-effect-value-idleSway').innerText(),'0%');
  // A head slider edit shows Custom and keeps everything else; restoring it matches the preset again.
  await preset.selectOption('action');
  await slider('turnLean').fill('40');await slider('turnLean').dispatchEvent('input');
  await page.waitForFunction(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId).effects.turnLean===.4);
  assert.equal(await preset.inputValue(),'custom');
  const custom=await effects();assert.equal(custom.headLag,PRESETS.action.headLag);assert.equal(custom.acceleration,PRESETS.action.acceleration);
  await slider('turnLean').fill('90');await slider('turnLean').dispatchEvent('input');assert.equal(await preset.inputValue(),'action');
  await slider('idleSway').fill('30');await slider('idleSway').dispatchEvent('input');
  await page.waitForFunction(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId).effects.idleSway===.3);
  assert.equal(await preset.inputValue(),'custom');
  // A camera saved before the head model existed shows the default head strengths and matches Realistic.
  await page.evaluate(()=>{const c=FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId);c.effects={enabled:true,intensity:.35,turbulence:.5,groundRoll:.35,touchdown:.5,acceleration:.35,engine:.2};});
  await select('aircraftExterior');await select('aircraftCockpit');
  assert.equal(await preset.inputValue(),'realistic');
  for(const key of HEAD)assert.equal(await slider(key).inputValue(),String(Math.round(PRESETS.realistic[key]*100)),'older '+key);
  await preset.selectOption('subtle');

  // Exterior and orbit views hide the head sliders; hidden head values do not turn the preset into Custom.
  for(const mode of ['aircraftExterior','orbit']){
   await select(mode);
   assert.equal(await group.isVisible(),false,mode);
   for(const key of BASE)assert.equal(await slider(key).isVisible(),true,mode+' '+key);
   await preset.selectOption('action');assert.equal((await effects()).headLag,PRESETS.action.headLag,mode);
   await page.evaluate(()=>{const c=FSCApp.state.working.profile.cameras.find(c=>c.id===effectCameraId);c.effects=Object.assign({},c.effects,{headLag:.12,idleSway:.7});});
   await select('aircraftCockpit');await select(mode);
   assert.equal(await preset.inputValue(),'action',mode);
   assert.match(await page.locator('#mc-effects-status').innerText(),/half strength/);
  }
  for(const mode of ['airport','flyby']){
   if(!await page.evaluate(mode=>FSCApp.state.working.profile.cameras.some(c=>c.mode===mode),mode))continue;
   await select(mode);assert.equal(await group.isVisible(),false,mode);
  }

  // Head strengths are saved with the camera.
  const persisted=await page.evaluate(async()=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('RevertChanges',{});return FSCApp.state.working.profile.cameras.find(c=>c.mode==='aircraftCockpit').effects;});
  for(const key of BASE.concat(HEAD))assert.equal(persisted[key],PRESETS.subtle[key],'persisted '+key);
  assert.deepEqual(errors,[]);console.log('Head movement sliders are cockpit-only, presets set them, older profiles get defaults, and values persist.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
