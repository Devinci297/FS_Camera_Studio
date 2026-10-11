const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1200,height:1100}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const code=fs.readFileSync('profiles/cessna-152-test.json','utf8');
  await page.locator('#mc-nav-profiles').click();await page.locator('#mc-import-profile').click();
  await page.getByText('Paste profile code instead',{exact:true}).click();await page.locator('#mc-dialog-body textarea').fill(code);
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  await page.getByText('Profile imported',{exact:true}).waitFor();
  const before=await page.evaluate(()=>({ids:FSCApp.state.profiles.map(p=>p.profileId),aircraft:FSCApp.state.aircraft.modelKey}));
  assert.ok(await page.getByRole('button',{name:'Copy for current aircraft',exact:true}).isVisible());
  await page.getByRole('button',{name:'Copy for current aircraft',exact:true}).click();
  await page.getByLabel('New profile name',{exact:true}).fill('152 views adapted to current aircraft');
  await page.getByRole('button',{name:'Create copy',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.working.profile.name==='152 views adapted to current aircraft');
  const copied=await page.evaluate(()=>JSON.parse(JSON.stringify(FSCApp.state.working.profile)));
  assert.equal(copied.aircraftMatch.modelKey,before.aircraft);assert.ok(!before.ids.includes(copied.profileId));
  const raw=JSON.parse(code),source=raw.profile||raw;
  assert.equal(copied.cameras.length,source.cameras.length);
  for(let i=0;i<source.cameras.length;i++){
   assert.deepEqual(copied.cameras[i].positionMetersFRD,source.cameras[i].positionMetersFRD);
   assert.deepEqual(copied.cameras[i].projection,source.cameras[i].projection);
   assert.notEqual(copied.cameras[i].id,source.cameras[i].id);
  }
  const imported=await page.evaluate(()=>FSCApp.state.profiles.find(p=>p.name==='Cessna 152 - Import test views'));
  const sourceId=imported?imported.profileId:await page.evaluate(ids=>FSCApp.state.profiles.find(p=>ids.includes(p.profileId)&&p.name.includes('152')).profileId,before.ids);
  const original=await page.evaluate(async id=>(await FSCApp.bridge.request('ExportProfile',{profileId:id})).result.code,sourceId);
  for(const [title,model] of [['Generic turboprop','vendor-turboprop'],['Unrelated airliner','vendor-airliner']]){
   await page.evaluate(({title,model})=>FSCDemo.loadAircraft(title,model),{title,model});
   await page.waitForFunction(model=>FSCApp.state.aircraft.modelKey===model,model);
   await page.locator('#mc-nav-profiles').click();
   await page.locator('[data-focus-key="copy-aircraft:'+sourceId+'"]').click();
   await page.getByLabel('New profile name',{exact:true}).fill(title+' copied views');
   await page.getByRole('button',{name:'Create copy',exact:true}).click();
   await page.waitForFunction(model=>FSCApp.state.working.profile.aircraftMatch.modelKey===model,model);
   assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras.length),source.cameras.length);
  }
  assert.equal(await page.evaluate(async id=>(await FSCApp.bridge.request('ExportProfile',{profileId:id})).result.code,sourceId),original,'Copying never rebinds or edits original');
  await page.locator('#mc-nav-profiles').click();await page.locator('[data-focus-key="copy-aircraft:'+sourceId+'"]').click();
  await page.evaluate(()=>FSCDemo.loadAircraft('Another aircraft','changed-during-dialog'));
  await page.waitForFunction(()=>FSCApp.state.aircraft.modelKey==='changed-during-dialog');
  await page.getByRole('button',{name:'Create copy',exact:true}).click();
  await page.getByText('The loaded aircraft changed. Close this dialog and try again.',{exact:true}).waitFor();
  assert.deepEqual(errors,[]);console.log('Import and profile-list cross-aircraft copies preserve views and originals, work for arbitrary models, and guard aircraft changes.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
