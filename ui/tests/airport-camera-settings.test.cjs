const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1450,height:1200}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const seed=await page.evaluate(()=>{
   const s=FSCApp.state,c=s.working.profile.cameras[0],g=s.working.profile.categories.find(g=>g.cameraIds.includes(c.id));
   Object.assign(c,{mode:'airport',airportIcao:'KAAA',worldOrigin:{latitude:47,longitude:-122,altitude:100},airportHeight:{category:'gates',baseAltitudeMeters:100,minimumMeters:0}});delete c.airportOverrides;
   g.name='Gates';g.airportIcao='KAAA';s.selection.cameraId=c.id;s.caps.modes.airport={available:true};
   Object.assign(s.working.settings.host,{globalAircraftTracking:'both',airportAutoZoomEnabled:true,airportAutoZoomFrameMeters:48,airportAutoZoomMinFovDeg:15,airportAutoZoomMaxFovDeg:80,airportAutoZoomResponseSeconds:1.4,airportCategoryHeights:{runways:4,gates:7,apron:5,taxi:6,tower:40}});
   FSCDemo.setOwnership('Observing');return {camera:c.id,host:JSON.stringify(s.working.settings.host)};
  });
  await page.locator('#mc-tab-behavior').click();await page.locator('#mc-camera-airport-settings').click();
  const body=page.locator('#mc-dialog-body'),tracking=body.getByRole('combobox',{name:'Aircraft tracking',exact:true}),zoom=body.getByRole('combobox',{name:'Auto zoom',exact:true});
  const horizon=body.getByRole('combobox',{name:'Keep distant views near the horizon',exact:true});
  assert.equal(await horizon.inputValue(),'global');assert.equal(await horizon.locator('option:checked').textContent(),'Global · On');
  assert.equal(await tracking.inputValue(),'global');assert.equal(await zoom.inputValue(),'global');
  const numeric=[['heightMeters','Height above feature',7,18],['autoZoomFrameMeters','Framing size',48,62],['autoZoomMinFovDeg','Closest zoom (minimum FOV)',15,20],['autoZoomMaxFovDeg','Widest zoom (maximum FOV)',80,75],['autoZoomResponseSeconds','Smoothing time',1.4,2.2]];
  for(const [,label,expected] of numeric){
   const input=body.getByRole('spinbutton',{name:label,exact:true});
   assert.equal(await input.isDisabled(),true,label+' inherits by default');assert.equal(Number(await input.inputValue()),expected);
  }
  await tracking.selectOption('heading');await zoom.selectOption('false');await horizon.selectOption('false');
  for(const [,label,,value] of numeric){
   await body.getByRole('checkbox',{name:'Use global '+label.toLowerCase(),exact:true}).uncheck();
   const input=body.getByRole('spinbutton',{name:label,exact:true});assert.equal(await input.isDisabled(),false);
   await input.evaluate((input,value)=>{input.value=String(value);input.dispatchEvent(new Event('change',{bubbles:true}));},value);
  }
  const expected={aircraftTracking:'heading',autoZoomEnabled:false,trackingHorizonGuard:false};numeric.forEach(([key,,,value])=>expected[key]=value);
  assert.deepEqual(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).airportOverrides,seed.camera),expected,'Each field supports an independent camera override');
  for(const [label,value,changedLabel,expectedValue] of [['Closest zoom (minimum FOV)',100,'Widest zoom (maximum FOV)',100],['Widest zoom (maximum FOV)',10,'Closest zoom (minimum FOV)',10]]){
   await body.getByRole('spinbutton',{name:label,exact:true}).evaluate((input,value)=>{input.value=String(value);input.dispatchEvent(new Event('change',{bubbles:true}));},value);
   assert.equal(Number(await body.getByRole('spinbutton',{name:changedLabel,exact:true}).inputValue()),expectedValue,'Changing zoom limits keeps the paired control consistent');
  }
  assert.equal(await page.evaluate(()=>JSON.stringify(FSCApp.state.working.settings.host)),seed.host,'Camera overrides leave global settings unchanged');
  await page.waitForTimeout(600);await page.locator('#mc-dialog-close').click();await page.locator('#mc-camera-airport-settings').click();
  assert.equal(await horizon.inputValue(),'false','Reopening retains explicit horizon guard opt-out');
  await horizon.selectOption('true');assert.equal(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).airportOverrides.trackingHorizonGuard,seed.camera),true);
  assert.equal(await tracking.inputValue(),'heading');assert.equal(await zoom.inputValue(),'false','Reopening retains explicit false override');
  await body.getByRole('checkbox',{name:'Use global framing size',exact:true}).check();
  assert.equal(Number(await body.getByRole('spinbutton',{name:'Framing size',exact:true}).inputValue()),48);
  assert.equal(await page.evaluate(id=>Object.hasOwn(FSCApp.state.working.profile.cameras.find(c=>c.id===id).airportOverrides,'autoZoomFrameMeters'),seed.camera),false,'Individual inheritance removes only that override');
  await body.getByRole('button',{name:'Use global settings for everything',exact:true}).click();
  assert.equal(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).airportOverrides,seed.camera),undefined);
  assert.equal(await tracking.inputValue(),'global');assert.equal(await zoom.inputValue(),'global');
  assert.equal(await horizon.inputValue(),'global','Reset restores horizon guard inheritance');
  for(const [,label] of numeric)assert.equal(await body.getByRole('spinbutton',{name:label,exact:true}).isDisabled(),true);
  assert.equal(await page.evaluate(()=>JSON.stringify(FSCApp.state.working.settings.host)),seed.host,'Resetting a camera does not reset global values');
  await page.locator('.mc-dialog').screenshot({path:'build/review/airport-camera-settings.png'});
  await page.locator('#mc-dialog-close').click();
  await page.locator('#mc-tab-position').click();
  assert.equal(Number(await page.locator('#mc-pose-y').inputValue()),7,'Pose editor displays effective inherited airport height');
  await page.locator('[data-axis="y"] .mc-direction-plus').click();
  const moved=Number(await page.locator('#mc-pose-y').inputValue());assert.ok(moved>7,'Up starts from inherited height instead of stale stored position');
  assert.equal(await page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).airportOverrides.heightMeters,seed.camera),moved,'Vertical adjustment creates a camera-specific height override');
  assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportCategoryHeights.gates),7,'Vertical adjustment keeps category default unchanged');
  await page.evaluate(()=>{
   window.airportPreviewCalls=[];const request=FSCApp.bridge.request.bind(FSCApp.bridge);
   FSCApp.bridge.request=async(action,payload)=>{if(action==='PreviewAirports'){airportPreviewCalls.push(payload);return {status:'Completed'};}if(action==='DiscoverAirports')return {status:'Completed'};return request(action,payload);};
  });
  await page.locator('#mc-add').click();await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();
  const useGlobal=page.getByRole('checkbox',{name:'Use global category heights',exact:true});assert.equal(await useGlobal.isChecked(),true);
  assert.equal(await page.locator('#mc-airport-height').isDisabled(),true);
  await page.locator('#mc-airport-codes').fill('KAAA');await page.locator('#mc-airport-preview').click();
  await page.waitForFunction(()=>airportPreviewCalls.length===1);
  const generated=await page.evaluate(()=>airportPreviewCalls[0].options);
  assert.equal(Object.hasOwn(generated,'height'),false,'Default generation does not replace category heights with one common height');
  assert.deepEqual(generated.categoryHeights,{runways:4,gates:7,apron:5,taxi:6,tower:40});
  await useGlobal.uncheck();await page.locator('#mc-airport-height').fill('12');await page.locator('#mc-airport-preview').click();
  await page.waitForFunction(()=>airportPreviewCalls.length===2);
  const custom=await page.evaluate(()=>airportPreviewCalls[1].options);assert.equal(custom.height,12);assert.equal(Object.hasOwn(custom,'categoryHeights'),false);
  await page.locator('#mc-dialog-close').click();await page.locator('#mc-nav-settings').click();await page.locator('#mc-settings-link-airport').click();
  const globalHorizon=page.locator('#mc-setting-airport-horizon-guard');assert.equal(await globalHorizon.isChecked(),true);
  await globalHorizon.uncheck();assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportTrackingHorizonGuard),false);
  await globalHorizon.check();assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportTrackingHorizonGuard),true);
  assert.deepEqual(errors,[]);console.log('Airport camera tracking, auto zoom and category height inheritance, independent overrides, reset, and unchanged global settings passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
