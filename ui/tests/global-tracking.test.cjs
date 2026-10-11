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
    await page.locator('#mc-nav-settings').click();
    assert.equal(await page.locator('#mc-setting-global-head-tracking').isChecked(),true);
    assert.equal(await page.locator('#mc-setting-global-aircraft-tracking').innerText(),'Heading + pitch');
   }
  }
  assert.deepEqual(errors,[]);console.log('Global tracking defaults, camera/profile independence, persistent settings, and shared controls passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
