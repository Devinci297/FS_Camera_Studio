const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Temporary airport sets: the module generates views for a nearby airport without saved cameras. The library
// marks them "Temporary — Save to keep"; Save stores them as a normal airport set with the same camera IDs.
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const url=surface=>'file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface='+surface;
  const page=await browser.newPage({viewport:{width:1300,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(url('main'));
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  assert.equal(await page.locator('.mc-airport-tree').count(),0,'Away from the airport there is no airport set');
  const aircraftCameras=await page.evaluate(()=>FSCApp.state.working.profile.cameras.length);
  // Arrive at the demo airport: its temporary set loads under Airport like a saved one.
  await page.evaluate(()=>{window.sent=[];const send=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=(a,p)=>{sent.push(a);return send(a,p);};FSCDemo.arriveAtAirport(true);});
  await page.locator('.mc-airport-tree').waitFor();
  assert.match(await page.locator('.mc-airport-tree>.mc-tree-scope-toggle').innerText(),/Airport KDEM/);
  const notice=page.locator('.mc-airport-tree .mc-airport-temporary');
  assert.ok(await notice.isVisible());assert.match(await notice.innerText(),/Temporary — Save to keep/);
  assert.equal(await notice.getAttribute('role'),'status');
  const categories=await page.locator('.mc-airport-tree .mc-category-head').allInnerTexts();
  for(const name of ['Runways','Spotting','Gates','Tower'])assert.ok(categories.some(text=>text.includes(name)),name+' subcategory is listed');
  const before=await page.evaluate(()=>FSCApp.state.working.profile.cameras.filter(c=>c.airportIcao==='KDEM').map(c=>c.id));
  assert.equal(before.length,6);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras.length),aircraftCameras+6);
  assert.equal(await page.locator('#mc-save-state').getAttribute('data-dirty'),'false','A temporary set is not an unsaved edit');
  // The profiles page summarises the same state.
  await page.locator('#mc-nav-profiles').click();
  assert.match(await page.locator('#mc-airports-summary').innerText(),/Temporary airport cameras: KDEM[\s\S]*Temporary — Save to keep/);
  await page.locator('#mc-nav-cameras').click();
  // Settings > Airport defaults turns automatic generation off and on.
  await page.locator('#mc-nav-settings').click();await page.locator('#mc-settings-link-airport').click();
  const toggle=page.locator('#mc-setting-airport-auto-generate');
  assert.ok(await toggle.isVisible());assert.equal(await toggle.isChecked(),true);
  assert.equal(await page.locator('#mc-settings-section-airport #mc-setting-airport-auto-generate').count(),1,'The switch lives in the Airport section');
  await toggle.click();
  await page.waitForFunction(()=>!FSCApp.state.airports.loaded);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportAutoGenerate),false);
  await page.locator('#mc-nav-cameras').click();assert.equal(await page.locator('.mc-airport-tree').count(),0,'Off: no temporary set');
  await page.locator('#mc-nav-settings').click();await toggle.click();
  await page.waitForFunction(()=>FSCApp.state.airports.loaded==='KDEM');
  await page.locator('#mc-nav-cameras').click();await page.locator('.mc-airport-tree .mc-airport-temporary').waitFor();
  // Save converts it into a saved airport set with the same camera IDs.
  const save=page.getByRole('button',{name:'Save KDEM airport cameras',exact:true});
  assert.ok(await save.isEnabled());await save.click();
  await page.waitForFunction(()=>FSCApp.state.airports.transient===false&&!document.querySelector('.mc-airport-temporary'));
  assert.ok(await page.evaluate(()=>sent.includes('SaveTemporaryAirport')));
  assert.ok(!(await page.evaluate(()=>sent.includes('CommitChanges'))),'Save uses the airport save, not Save all changes');
  assert.match(await page.locator('.mc-airport-tree>.mc-tree-scope-toggle').innerText(),/Airport KDEM/);
  const after=await page.evaluate(()=>FSCApp.state.working.profile.cameras.filter(c=>c.airportIcao==='KDEM').map(c=>c.id));
  assert.deepEqual(after,before);
  assert.ok(await page.evaluate(ids=>ids.every(id=>FSCApp.state.working.hasCommittedVersion[id]),after),'Saved cameras have a stored version');
  await page.locator('#mc-nav-profiles').click();assert.match(await page.locator('#mc-airports-summary').innerText(),/Loaded airport: KDEM/);
  assert.doesNotMatch(await page.locator('#mc-airports-summary').innerText(),/Temporary/);
  // Generator offers the new Spotting choice.
  await page.locator('#mc-nav-cameras').click();await page.locator('#mc-add').click();await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();
  assert.equal(await page.locator('#mc-airport-spotting').getAttribute('aria-checked'),'true');
  await page.locator('#mc-dialog-close').click();
  // Mini panel: the temporary airport group is marked.
  const mini=await browser.newPage({viewport:{width:1300,height:300}});mini.on('pageerror',e=>errors.push(e.message));
  await mini.goto(url('mini')+'&demoAirport=1');await mini.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot&&FSCApp.state.airports.loaded==='KDEM');
  assert.equal(await mini.evaluate(()=>FSCApp.state.airports.transient),true);
  await mini.waitForFunction(()=>document.querySelector('.mc-dock-airport'));
  assert.equal(await mini.locator('.mc-dock-airport').getAttribute('data-temporary'),'true');
  assert.match(await mini.locator('.mc-dock-airport').getAttribute('aria-label'),/KDEM, temporary/);
  assert.deepEqual(errors,[]);
  console.log('Temporary airport set: library state, Settings switch, Save with stable IDs, generator Spotting choice and mini marker passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
