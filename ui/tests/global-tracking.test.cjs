const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1450,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  for(const surface of ['main','mini']){
   await page.goto((surface==='mini'?url.replace(/main\.html$/,'mini.html'):url)+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>{FSCApp.state.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();});
   const prefix=surface==='main'?'mc-editor-mouse-look-':'mc-mouse-look-',head=page.locator('#'+prefix+'head'),track=page.locator('#'+prefix+'aircraft');
   assert.equal(await head.getAttribute('aria-pressed'),'false','Global head tracking defaults off');
   assert.match(await track.innerText(),/Heading \+ pitch/,'Global aircraft tracking defaults both');
   assert.ok(await track.isVisible(),'Global aircraft control remains visible from cockpit');
   await head.click();
   await track.click();await page.getByRole('button',{name:'Fixed direction',exact:true}).click();
   await page.waitForTimeout(600);
   const before=await page.evaluate(()=>({host:FSCApp.state.working.settings.host,profiles:FSCApp.state.profiles,profile:FSCApp.state.working.profile.profileId,cameras:JSON.stringify(FSCApp.state.working.profile.cameras)}));
   assert.equal(before.host.globalHeadTrackingEnabled,true);assert.equal(before.host.globalAircraftTracking,'off');
   await page.evaluate(()=>{
    const s=FSCApp.state,c=s.working.profile.cameras[1];c.trackingPolicy='off';c.aircraftTracking='both';
    s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');
   });
   assert.equal(await head.getAttribute('aria-pressed'),'true','Per-camera legacy value cannot reset global head setting');
   assert.match(await track.innerText(),/Off/,'Per-camera legacy value cannot reset global aircraft setting');
   const other=before.profiles.find(p=>p.profileId!==before.profile);
   await page.evaluate(async id=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('OpenProfile',{profileId:id,discard:true});},other.profileId);
   await page.waitForFunction(id=>FSCApp.state.working.profile.profileId===id,other.profileId);
   assert.equal(await head.getAttribute('aria-pressed'),'true','Profile switch preserves global head setting');
   assert.match(await track.innerText(),/Off/,'Profile switch preserves global aircraft setting');
   await track.click();await page.getByRole('button',{name:'Track heading and pitch',exact:true}).click();
   await page.waitForTimeout(500);
   assert.match(await track.innerText(),/Heading \+ pitch/);
   if(surface==='main'){
    await page.locator('#mc-nav-settings').click();await page.locator('#mc-settings-link-tracking').click();
    assert.equal(await page.locator('#mc-setting-global-head-tracking').isChecked(),true);
    const choices=page.getByRole('group',{name:'Airport camera tracking',exact:true});
    assert.equal(await choices.getByRole('button',{name:'Heading + pitch',exact:true}).getAttribute('aria-pressed'),'true');
    for(const [value,label,toolbar] of [['off','Off','Off'],['heading','Heading only','Heading'],['both','Heading + pitch','Heading + pitch']]){
     await choices.getByRole('button',{name:label,exact:true}).click();
     assert.equal(await choices.locator('[aria-pressed="true"]').count(),1);
     assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.globalAircraftTracking),value);
     assert.equal(await track.innerText(),'Aircraft: '+toolbar);
     assert.equal(await page.locator('#mc-setting-global-head-tracking').isChecked(),true,'Airport setting leaves head tracking unchanged');
     await page.waitForTimeout(400);
    }
    await choices.getByRole('button',{name:'Heading only',exact:true}).click();await page.waitForTimeout(500);
    await page.evaluate(async()=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,300));await FSCApp.bridge.request('RevertChanges',{});});
    await page.waitForFunction(()=>FSCApp.state.working.settings.host.globalAircraftTracking==='heading');
    assert.equal(await choices.getByRole('button',{name:'Heading only',exact:true}).getAttribute('aria-pressed'),'true');
    await page.locator('#mc-settings-link-airport').click();
    const zoom=page.locator('#mc-setting-airport-autozoom'),fields=page.locator('#mc-autozoom-options input');
    const heights={runways:4,gates:7,apron:8,taxi:6,tower:45};
    for(const [key,value] of Object.entries(heights)){
     const height=page.locator('#mc-setting-airport-height-'+key);
     assert.equal(await height.isDisabled(),false,'Height defaults remain available when auto zoom is off');
     assert.equal(Number(await height.inputValue()),key==='tower'?35:3);
     await height.evaluate((input,value)=>{input.value=String(value);input.dispatchEvent(new Event('change',{bubbles:true}));},value);
    }
    const tower=page.locator('#mc-setting-airport-height-tower');
    for(const [value,expected] of [[-5,0],[1200,1000],['',35],[45,45]]){
     await tower.evaluate((input,value)=>{input.value=String(value);input.dispatchEvent(new Event('change',{bubbles:true}));},value);
     assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportCategoryHeights.tower),expected);
    }
    assert.equal(await zoom.isChecked(),false,'Auto zoom is opt-in');
    assert.equal(await fields.count(),4);
    for(const field of await fields.all())assert.equal(await field.isDisabled(),true,'Inactive auto zoom fields are disabled');
    await zoom.check();
    for(const field of await fields.all())assert.equal(await field.isDisabled(),false);
    const setZoom=async(key,value)=>{
     await page.locator('#mc-setting-'+key).evaluate((input,value)=>{input.value=String(value);input.dispatchEvent(new Event('change',{bubbles:true}));},value);
     return page.evaluate(key=>FSCApp.state.working.settings.host[key],key);
    };
    assert.equal(await setZoom('airportAutoZoomFrameMeters',-10),5,'Framing size clamps at lower bound');
    assert.equal(await setZoom('airportAutoZoomFrameMeters',999),200,'Framing size clamps at upper bound');
    assert.equal(await setZoom('airportAutoZoomFrameMeters',''),40,'Empty framing size uses the default');
    assert.equal(await setZoom('airportAutoZoomResponseSeconds',0),0.1,'Zoom smoothing cannot be instantaneous');
    assert.equal(await setZoom('airportAutoZoomResponseSeconds',99),5);
    assert.equal(await setZoom('airportAutoZoomMinFovDeg',100),100);
    assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportAutoZoomMaxFovDeg),100,'Increasing minimum expands maximum to keep valid limits');
    assert.equal(await setZoom('airportAutoZoomMaxFovDeg',6),6);
    assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportAutoZoomMinFovDeg),6,'Decreasing maximum lowers minimum to keep valid limits');
    await setZoom('airportAutoZoomMaxFovDeg',85);await setZoom('airportAutoZoomMinFovDeg',12);await setZoom('airportAutoZoomFrameMeters',55);await setZoom('airportAutoZoomResponseSeconds',1.7);
    await page.locator('#mc-settings-link-tracking').click();await choices.getByRole('button',{name:'Off',exact:true}).click();
    assert.match(await page.locator('#mc-airport-autozoom-status').innerText(),/waiting/i,'Tracking off explains why auto zoom is inactive');
    assert.equal(await zoom.isChecked(),true,'Tracking off preserves the auto zoom preference');
    await choices.getByRole('button',{name:'Heading only',exact:true}).click();
    await page.locator('#mc-settings-link-airport').click();await zoom.uncheck();
    assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.airportAutoZoomFrameMeters),55,'Turning off retains configured limits');
    await zoom.check();await page.waitForTimeout(500);
    await page.evaluate(async id=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,300));await FSCApp.bridge.request('OpenProfile',{profileId:id,discard:true});},before.profile);
    await page.waitForFunction(id=>FSCApp.state.working.profile.profileId===id,before.profile);
    assert.deepEqual(await page.evaluate(()=>{const h=FSCApp.state.working.settings.host;return [h.globalAircraftTracking,h.airportAutoZoomEnabled,h.airportAutoZoomFrameMeters,h.airportAutoZoomMinFovDeg,h.airportAutoZoomMaxFovDeg,h.airportAutoZoomResponseSeconds];}),['heading',true,55,12,85,1.7],'Global auto zoom persists across save and aircraft changes');
    assert.deepEqual(await page.evaluate(()=>FSCApp.state.working.settings.host.airportCategoryHeights),heights,'Category heights persist across save and aircraft changes');
    await page.locator('#mc-settings-link-tracking').click();
    for(const width of [1450,560]){
     await page.setViewportSize({width,height:1000});
     const layout=await page.locator('#mc-global-tracking-settings').evaluate(group=>{
      const parent=group.getBoundingClientRect(),items=Array.from(group.querySelectorAll('.mc-tracking-choice,.mc-autozoom-field')).map(el=>el.getBoundingClientRect());
      return {fits:items.every(r=>r.left>=parent.left-1&&r.right<=parent.right+1),overlap:items.some((a,i)=>items.slice(i+1).some(b=>Math.min(a.right,b.right)>Math.max(a.left,b.left)+1&&Math.min(a.bottom,b.bottom)>Math.max(a.top,b.top)+1))};
     });
     assert.equal(layout.fits,true,'Tracking controls fit the settings section at '+width+'px');
     assert.equal(layout.overlap,false,'Tracking controls do not overlap at '+width+'px');
     await page.setViewportSize({width,height:2200});
     await page.locator('#mc-global-tracking-settings').screenshot({path:'build/review/global-airport-tracking-settings-'+width+'.png'});
    }
    await page.setViewportSize({width:1450,height:2200});
    await page.locator('#mc-global-tracking-settings').screenshot({path:'build/review/global-airport-tracking-settings.png'});
    await page.setViewportSize({width:1450,height:1000});
   }
  }
  assert.deepEqual(errors,[]);console.log('Global tracking and auto zoom defaults, limits, persistence across profiles, responsive layout, and shared controls passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
