const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1300,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const aircraftCount=await page.evaluate(()=>{
   const s=FSCApp.state,p=s.working.profile,count=p.cameras.length;
   const groups=['Runways','Tower'].map(name=>({id:crypto.randomUUID(),name,airportIcao:'KAAA',cameraIds:[]}));
   groups.forEach(g=>{const c=JSON.parse(JSON.stringify(p.cameras[0]));c.id=crypto.randomUUID();c.name=g.name+' shared';c.mode='airport';c.airportIcao='KAAA';c.categoryId=g.id;g.cameraIds.push(c.id);p.cameras.push(c);});
   p.categories.push(...groups);s.selection.cameraId=groups[0].cameraIds[0];s.airports.loaded='KAAA';s.airports.scope='global';s.working.dirty={profile:true,aircraftProfile:false,airport:true};s.caps.modes.airport={available:true};
   s.airportPreviews.KAAA={icao:'KAAA',profile:{categories:groups,cameras:p.cameras.filter(c=>c.airportIcao)}};
   window.calls=[];const request=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=async(a,p)=>{calls.push({a,p});if(['SaveAirportPreview','DiscoverAirports','OpenProfile'].includes(a))return {status:'Completed'};return request(a,p);};
   FSCDemo.setOwnership('Observing');return count;
  });
  assert.equal(await page.locator('#mc-default-view').isDisabled(),true,'Shared airport cannot become aircraft profile default');
  await page.locator('#mc-camera-menu').click();await page.getByRole('button',{name:'Move to category',exact:true}).click();
  assert.deepEqual(await page.locator('#mc-dialog-body select option').allTextContents(),['Tower'],'Airport move targets stay within airport library');
  await page.locator('#mc-dialog-close').click();await page.locator('#mc-nav-profiles').click();
  const current=await page.locator('#mc-profiles-list .mc-profile-entry[data-current="true"] small').innerText();assert.ok(current.includes(aircraftCount+' cameras'),'Aircraft count excludes shared airport composition');assert.ok(!current.includes('unsaved'),'Airport edits do not mark aircraft profile dirty');
  await page.locator('#mc-export-profile').click();assert.match(await page.locator('#mc-dialog-body').innerText(),/Shared airport cameras are stored separately/);assert.equal(await page.getByRole('button',{name:'Export with edits',exact:true}).count(),0);await page.locator('#mc-dialog-close').click();
  await page.locator('#mc-nav-cameras').click();await page.locator('#mc-add').click();await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();assert.match(await page.locator('#mc-dialog-body').innerText(),/shared by every aircraft/);
  const replace=page.locator('#mc-airport-results>section>label input[type=checkbox]');await replace.check();
  await page.getByRole('button',{name:'Save selected views',exact:true}).click();
  assert.equal(await page.evaluate(()=>calls.filter(c=>c.a==='SaveAirportPreview').length),0,'Replacement needs explicit confirmation');
  await page.locator('.mc-airport-replace-confirmation').getByRole('button',{name:'Cancel',exact:true}).click();
  assert.equal(await page.evaluate(()=>calls.filter(c=>c.a==='SaveAirportPreview').length),0);
  await page.getByRole('button',{name:'Save selected views',exact:true}).click();
  await page.getByRole('button',{name:'Replace views and discard airport edits',exact:true}).click();
  await page.waitForFunction(()=>calls.some(c=>c.a==='SaveAirportPreview'));
  const saved=await page.evaluate(()=>calls.find(c=>c.a==='SaveAirportPreview').p);assert.equal(saved.icao,'KAAA');assert.equal(saved.replace,true);assert.equal(saved.discardAirportEdits,true);assert.equal(saved.cameraIds.length,2);
  await page.locator('#mc-dialog-close').click();await page.locator('#mc-nav-profiles').click();await page.getByRole('button',{name:/^Open /}).first().click();await page.waitForFunction(()=>calls.some(c=>c.a==='OpenProfile'));assert.equal(await page.locator('#mc-dialog-shade').isVisible(),false,'Airport-only edits must not prompt discard on aircraft switch');
  assert.deepEqual(errors,[]);console.log('Shared airport UI boundaries, aircraft-only count/export, and explicit regeneration discard consent passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
