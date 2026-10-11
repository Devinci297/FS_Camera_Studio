const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict');const path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 const page=await browser.newPage({viewport:{width:1100,height:1000}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
 await page.waitForFunction(()=>window.FSCApp.state.snapshot);
 await page.locator('#mc-nav-settings').click();
 assert.equal(await page.locator('#mc-qual-run').isVisible(),false,'manual axis wizard is removed');
 assert.equal(await page.locator('#mc-setting-unqualified').isVisible(),false);
 assert.match(await page.locator('#mc-qual-state').innerText(),/automatically/);
 await page.locator('#mc-nav-cameras').click();
 const xml='<FSData><Airport ident="KAAA" name="Test airport" lat="40" lon="-90" alt="100M"><Runway lat="40" lon="-90" alt="100M" heading="90" length="2000M" width="40M" number="09" designator="LEFT"/><TaxiwaySign lat="40.001" lon="-90" heading="45" label="A / B"/><TaxiwayParking type="GATE_SMALL" name="GATE_A" number="1" lat="40.002" lon="-90" heading="90"/><TaxiwayParking name="Missing location" number="2"/><TaxiwayPoint index="0" lat="40" lon="-90"/><TaxiwayPoint index="1" lat="40.001" lon="-90"/><TaxiwayPath type="TAXI" start="0" end="1" width="20M"/></Airport></FSData>';
 const parsed=await page.evaluate(text=>FSCApp.parseAirportXml(text),xml);
 assert.equal(parsed.parking[0].name,'Gate A1');assert.equal(parsed.parking[0].key,'12:1');
 const suffixXml=xml.replace('name="GATE_A" number="1"','name="GATE" number="12" suffix="GATE_B"');
 const suffixParsed=await page.evaluate(text=>FSCApp.parseAirportXml(text),suffixXml);
 assert.equal(suffixParsed.parking[0].name,'Gate 12B');assert.equal(suffixParsed.parking[0].key,'10:12:13');
 assert.equal(parsed.icao,'KAAA');assert.equal(parsed.runways[0].primary,'09L');assert.equal(parsed.runways[0].secondary,'27R');assert.equal(parsed.signs.length,1);assert.equal(parsed.parking[0].gate,true);assert.equal(parsed.parking[1].north,NaN);
 await page.evaluate(()=>{
   const app=FSCApp,S=app.state;window.airportCalls=[];const original=app.bridge.request;
   app.bridge.request=async(action,payload)=>{
     if(!/Airport/.test(action))return original(action,payload);
     window.airportCalls.push({action,payload});
     if(action==='SaveAirportPreview'){S.completedCommits['airport-test-save']={status:'Committed'};return {status:'Pending',requestId:'airport-test-save'};}
     return {status:'Completed'};
   };
   const cam=JSON.parse(JSON.stringify(S.working.profile.cameras[0]));cam.id='aaaaaaaa-aaaa-5aaa-aaaa-aaaaaaaaaaaa';cam.name='09 threshold';cam.categoryId='bbbbbbbb-bbbb-5bbb-abbb-bbbbbbbbbbbb';cam.airportIcao='KAAA';cam.mode='airport';cam.viewId=0;
   const cat={id:cam.categoryId,name:'Runways',airportIcao:'KAAA',cameraIds:[cam.id],defaultCameraId:cam.id};
   S.airportPreviews.KAAA={icao:'KAAA',profile:{cameras:[cam],categories:[cat]},notes:['No taxi-sign positions supplied; using offset taxiway intersections.']};
   S.airports={loaded:'KAAA',nearby:[{icao:'KAAA',name:'Test airport',distanceNm:0.2}],facilities:[{icao:'KAAA'}],radiusNm:25};
 });
 await page.locator('#mc-add').click();
 for(const name of ['New camera','New category','Create airport cameras']) assert.ok(await page.getByRole('button',{name,exact:true}).isVisible());
 assert.equal(await page.evaluate(()=>airportCalls.length),0,'Opening the library menu does not query scenery');
 await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();
 assert.equal(await page.evaluate(()=>airportCalls.filter(c=>c.action==='DiscoverAirports').length),1);
 assert.ok(await page.getByRole('dialog').isVisible());
 assert.ok(await page.getByText('No taxi-sign positions supplied; using offset taxiway intersections.',{exact:true}).isVisible());
 await page.getByRole('button',{name:'Nearest: KAAA',exact:true}).click();assert.equal(await page.locator('#mc-airport-codes').inputValue(),'KAAA');
 await page.getByRole('button',{name:'Save selected views',exact:true}).click();
 await page.getByRole('button',{name:'Saved — update selected views',exact:true}).waitFor();
 const save=await page.evaluate(()=>airportCalls.find(c=>c.action==='SaveAirportPreview'));assert.equal(save.payload.icao,'KAAA');assert.equal(save.payload.cameraIds.length,1);assert.equal(save.payload.replace,false);
 await page.screenshot({path:'build/review/airport-generator.png'});
 await page.locator('#mc-airport-codes').fill('KAAA KBBB');await page.locator('#mc-airport-preview').click();
 const preview=await page.evaluate(()=>airportCalls.find(c=>c.action==='PreviewAirports'));assert.deepEqual(preview.payload.codes,['KAAA','KBBB']);assert.equal(preview.payload.options.offset,12);
 await page.locator('#mc-dialog-close').click();await page.locator('#mc-nav-cameras').click();
 // Existing creation actions still open their original forms after the airport dialog closes.
 for(const [name,title] of [['New camera','Add camera'],['New category','Add category']]) {
  await page.locator('#mc-add').click();await page.getByRole('button',{name,exact:true}).click();
  assert.equal(await page.locator('#mc-dialog-title').innerText(),title);
  assert.equal(await page.locator('#mc-dialog-shade').evaluate(e=>e.classList.contains('mc-airport-shade')),false);
  await page.locator('#mc-dialog-close').click();
 }
 assert.deepEqual(errors,[]);await browser.close();console.log('Airport UI: library + menu, XML parsing, feature preview, selective save, ICAO batch and existing camera/category forms passed: '+(process.argv[2]||'source'));
})().catch(e=>{console.error(e);process.exit(1);});
