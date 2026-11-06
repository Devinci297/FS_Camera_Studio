const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const id=await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode==='aircraftExterior');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');return c.id;});
  const cam=()=>page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id),id);
  // Mode picker: the mounted action cam sits beside the other aircraft-relative modes.
  await page.locator('#mc-tab-behavior').click();
  assert.equal(await page.locator('#mc-mount-settings').isHidden(),true,'mount settings only for mounted cameras');
  await page.locator('#mc-view-type').click();
  const modes=await page.locator('#mc-dialog-shade .mc-dialog button[aria-pressed]').allTextContents();
  assert.deepEqual(modes.map(t=>t.trim()),['Cockpit','Aircraft exterior','Orbit','Fly-by','Mounted action cam']);
  await page.getByRole('button',{name:'Mounted action cam',exact:true}).click();
  let saved=await cam();
  assert.equal(saved.mode,'mounted');assert.equal(saved.gimbal,'locked');assert.equal(saved.mountVibration,0.3);assert.equal(saved.interaction,false);
  assert.equal(await page.locator('#mc-view-type').innerText(),'Mounted action cam');
  assert.equal(await page.evaluate(()=>FSCApp.state.caps.modes.mounted.available),true,'the demo module can preview mounted cams');
  assert.equal(await page.locator('#mc-view-in-sim').isEnabled(),true);
  // Behavior: gimbal choices and the vibration slider; follow horizon is superseded by the gimbal.
  await page.locator('#mc-tab-behavior').click();
  assert.equal(await page.locator('#mc-mount-settings').isVisible(),true);
  const gimbal=page.locator('#mc-mount-gimbal');
  assert.deepEqual((await gimbal.locator('button').allTextContents()).map(t=>t.trim()),['Locked','Level horizon','Heading only']);
  assert.equal(await gimbal.locator('[aria-pressed="true"]').innerText(),'Locked');
  assert.equal(await page.locator('#mc-follow-horizon-range').isDisabled(),true);assert.equal(await page.locator('#mc-follow-horizon-number').isDisabled(),true);
  assert.match(await page.locator('#mc-follow-horizon-note').textContent(),/gimbal/);
  await page.getByRole('button',{name:'Gimbal: Level horizon',exact:true}).click();
  assert.equal((await cam()).gimbal,'horizon');assert.equal(await gimbal.locator('[aria-pressed="true"]').innerText(),'Level horizon');
  await page.getByRole('button',{name:'Gimbal: Heading only',exact:true}).click();
  assert.equal((await cam()).gimbal,'yaw');
  const slider=page.getByRole('slider',{name:'Airframe vibration',exact:true}),number=page.locator('#mc-mount-vibration-number');
  assert.equal(await slider.count(),1);assert.deepEqual(await slider.evaluate(el=>[el.min,el.max,el.step]),['0','1','0.05']);
  assert.equal(await slider.inputValue(),'0.3');assert.equal(await number.inputValue(),'0.3');
  await number.fill('0.65');await number.press('Tab');
  assert.equal((await cam()).mountVibration,0.65);assert.equal(await slider.inputValue(),'0.65');
  await number.fill('4');await number.press('Tab');
  assert.equal((await cam()).mountVibration,1,'clamped to 1');
  await slider.evaluate(el=>{el.value='0';el.dispatchEvent(new Event('input',{bubbles:true}));});
  assert.equal((await cam()).mountVibration,0);assert.equal(await number.inputValue(),'0');
  // Aircraft tracking does not apply: the effective tracking line says so.
  assert.match(await page.locator('.mc-effective-tracking').first().innerText(),/Mounted · aircraft tracking excluded/);
  // Motion tab: no automatic motion or flight effects; the transition stays editable.
  await page.locator('#mc-tab-motion').click();
  assert.match(await page.locator('#mc-external-motion').innerText(),/Rigid on the airframe/);
  assert.equal(await page.locator('#mc-effects-enabled').isDisabled(),true);
  assert.equal(await page.locator('[data-effect]').first().isDisabled(),true);
  assert.equal(await page.locator('#mc-delay').isEnabled(),true);
  assert.equal(await page.getByRole('spinbutton',{name:'Seconds per orbit',exact:true}).count(),0);
  // The demo module accepts and persists the camera through save and revert.
  await page.waitForTimeout(800);
  const persisted=await page.evaluate(async id=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('RevertChanges',{});const c=FSCApp.state.working.profile.cameras.find(c=>c.id===id);return {mode:c.mode,gimbal:c.gimbal,mountVibration:c.mountVibration,dirty:FSCApp.state.working.dirty.profile};},id);
  assert.deepEqual(persisted,{mode:'mounted',gimbal:'yaw',mountVibration:0,dirty:false});
  // A live recall works in the demo module.
  assert.equal((await page.evaluate(id=>FSCApp.bridge.request('ActivateCamera',{cameraId:id}),id)).status,'Accepted');
  await page.waitForFunction(id=>FSCApp.state.active&&FSCApp.state.active.cameraId===id&&FSCApp.state.ownership.state==='Active',id,{timeout:5000});
  // Leaving the mode hides the section and gives follow horizon back; the gimbal choice is kept for later.
  await page.locator('#mc-tab-behavior').click();await page.locator('#mc-view-type').click();await page.getByRole('button',{name:'Aircraft exterior',exact:true}).click();
  assert.equal(await page.locator('#mc-mount-settings').isHidden(),true);assert.equal(await page.locator('#mc-follow-horizon-range').isEnabled(),true);
  assert.equal((await cam()).gimbal,'yaw');
  // Schema 3 profile codes import as schema 4; a schema 4 code with a mounted camera round-trips.
  const imported=await page.evaluate(async()=>{
   const source=JSON.parse(JSON.stringify(FSCApp.state.working.profile));
   source.schemaVersion=3;source.name='Legacy mounted';
   source.cameras=source.cameras.filter(c=>c.mode==='aircraftExterior').slice(0,2);
   source.categories=source.categories.map(cat=>Object.assign({},cat,{cameraIds:cat.cameraIds.filter(id=>source.cameras.some(c=>c.id===id))})).filter(cat=>cat.cameraIds.length);
   source.cameras.forEach(c=>{delete c.airportIcao;delete c.gimbal;delete c.mountVibration;});
   source.cameras[0].mode='mounted';source.defaultCameraId=source.cameras[0].id;
   const reply=await FSCApp.bridge.request('ImportProfile',{code:JSON.stringify(source)});
   if(reply&&reply.status==='Rejected')return {status:reply.status,reason:reply.reason};
   await new Promise(r=>setTimeout(r,300));
   const list=(FSCApp.state.profiles||[]).filter(p=>p.name==='Legacy mounted');if(!list.length)return {missing:true};
   const doc=JSON.parse((await FSCApp.bridge.request('ExportProfile',{profileId:list[list.length-1].profileId,working:false})).result.code);
   const mounted=doc.cameras.find(c=>c.mode==='mounted');
   return {schemaVersion:doc.schemaVersion,gimbal:mounted.gimbal,mountVibration:mounted.mountVibration};
  });
  assert.deepEqual(imported,{schemaVersion:4,gimbal:'locked',mountVibration:0.3});
  assert.deepEqual(errors,[]);console.log('Mounted action cam mode picker, gimbal and vibration controls, gating, persistence and schema 4 import passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
