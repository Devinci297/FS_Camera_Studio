const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const cockpit=await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode==='aircraftCockpit');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');return c.id;});
  await page.locator('#mc-tab-behavior').click();
  assert.equal(await page.locator('#mc-level').count(),0,'the disabled checkbox is gone');
  const slider=page.getByRole('slider',{name:'Follow horizon',exact:true}),number=page.locator('#mc-follow-horizon-number');
  assert.equal(await slider.count(),1);assert.equal(await number.count(),1);
  assert.equal(await page.evaluate(()=>FSCApp.state.caps.followHorizon),true);
  assert.equal(await slider.isEnabled(),true);assert.equal(await number.isEnabled(),true);
  assert.equal(await slider.inputValue(),'1');assert.equal(await number.inputValue(),'1');
  assert.deepEqual(await slider.evaluate(el=>[el.min,el.max,el.step]),['0','1','0.05']);
  assert.match(await page.locator('#mc-follow-horizon-note').textContent(),/1 = banks with the aircraft, 0 = horizon stays level/);
  await slider.hover();const tip=page.locator('#mc-control-tooltip');await tip.waitFor();
  assert.match(await tip.textContent(),/1 = banks fully with the aircraft, 0 = the horizon stays level/);
  await page.mouse.move(1,1);
  // Number entry and slider both edit the working camera and stay in sync.
  await number.fill('0.25');await number.press('Tab');
  assert.equal(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).followHorizon,cockpit),0.25);
  assert.equal(await slider.inputValue(),'0.25');
  await number.fill('7');await number.press('Tab');
  assert.equal(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).followHorizon,cockpit),1,'clamped to 1');
  await slider.evaluate(el=>{el.value='0';el.dispatchEvent(new Event('input',{bubbles:true}));});
  assert.equal(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).followHorizon,cockpit),0);
  assert.equal(await number.inputValue(),'0');
  await page.waitForTimeout(800);
  const persisted=await page.evaluate(async id=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('RevertChanges',{});return FSCApp.state.working.profile.cameras.find(c=>c.id===id).followHorizon;},cockpit);
  assert.equal(persisted,0);
  assert.equal(await page.evaluate(id=>'levelHorizon' in FSCApp.state.working.profile.cameras.find(c=>c.id===id),cockpit),false);
  // World-fixed views keep a level horizon and cannot change it.
  await page.locator('#mc-add').click();await page.getByRole('button',{name:'External motion views',exact:true}).click();await page.waitForTimeout(600);
  await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode==='flyby');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');});
  await page.locator('#mc-tab-behavior').click();
  assert.equal(await slider.isDisabled(),true);assert.equal(await number.isDisabled(),true);
  assert.match(await page.locator('#mc-follow-horizon-note').textContent(),/World-fixed/);
  await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode==='orbit');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');});
  assert.equal(await slider.isEnabled(),true,'orbit views can level the horizon');
  // Schema 2 profile codes migrate: levelHorizon true -> 0, false or absent -> 1.
  const imported=await page.evaluate(async()=>{
   const source=JSON.parse(JSON.stringify(FSCApp.state.working.profile));
   source.schemaVersion=2;source.name='Legacy horizon';
   source.cameras=source.cameras.filter(c=>c.mode==='aircraftCockpit').slice(0,3);
   source.categories=source.categories.map(cat=>Object.assign({},cat,{cameraIds:cat.cameraIds.filter(id=>source.cameras.some(c=>c.id===id))})).filter(cat=>cat.cameraIds.length);
   source.cameras.forEach(c=>{delete c.followHorizon;delete c.airportIcao;});
   source.cameras[0].levelHorizon=true;source.cameras[1].levelHorizon=false;
   source.defaultCameraId=source.cameras[0].id;
   const reply=await FSCApp.bridge.request('ImportProfile',{code:JSON.stringify(source)});
   await new Promise(r=>setTimeout(r,300));
   return {status:reply&&reply.status,reason:reply&&reply.reason};
  });
  assert.notEqual(imported.status,'Rejected',String(imported.reason));
  const migrated=await page.evaluate(async()=>{
   const list=(FSCApp.state.profiles||[]).filter(p=>p.name==='Legacy horizon');
   if(!list.length)return null;
   const code=(await FSCApp.bridge.request('ExportProfile',{profileId:list[list.length-1].profileId,working:false})).result.code;
   const p=JSON.parse(code);return {schemaVersion:p.schemaVersion,values:p.cameras.map(c=>c.followHorizon),legacy:p.cameras.some(c=>'levelHorizon' in c)};
  });
  assert.ok(migrated,'the imported profile is listed');assert.equal(migrated.schemaVersion,4);assert.deepEqual(migrated.values,[0,1,1]);assert.equal(migrated.legacy,false);
  assert.deepEqual(errors,[]);console.log('Follow horizon slider, tooltip, persistence, world-view gating and schema 2 migration passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
