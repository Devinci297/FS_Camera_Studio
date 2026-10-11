const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const errors=[],page=await browser.newPage({viewport:{width:1450,height:1000}});page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  if(!process.env.FSCS_MINI_ONLY) {
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.locator('#mc-editor-mouse-look-head').click();
  await page.waitForFunction(()=>FSCApp.state.working.settings.host.globalHeadTrackingEnabled===true);
  await page.locator('#mc-editor-mouse-look').click();await page.getByRole('button',{name:'Return to saved view',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.working.profile.cameras[0].mouseLookOnRelease==='reset');await page.getByRole('button',{name:'Done',exact:true}).click();
  await page.locator('#mc-editor-mouse-look').click();await page.getByRole('button',{name:'Remember temporary look',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.working.profile.cameras[0].temporaryLookPolicy==='retain');await page.getByRole('button',{name:'Done',exact:true}).click();
  await page.keyboard.press('Control+s');
  await page.waitForFunction(()=>!FSCApp.state.pendingCommit&&!FSCApp.state.inflight&&!FSCApp.state.queue.length&&!FSCApp.state.working.dirty.profile);
  await page.evaluate(()=>new Promise(resolve=>{
    FSCApp.bridge.on('event',e=>{if(e.name==='Snapshot')resolve();});
    FSCApp.bridge.send('RequestSnapshot',{});
  }));
  // Let queued demo commit snapshots settle before replacing its data with a UI-only fixture.
  await page.waitForTimeout(600);
  // Reserve slots 1..20, then assign the unassigned camera without the legacy cap.
  // These synthetic cameras exist only in the UI fixture, not the demo repository.
  // Do not let a delayed demo snapshot replace the fixture after an unknown ID patch.
  await page.evaluate(()=>{FSCApp.bridge.request=async()=>({status:'Completed'});FSCApp.bridge.send=()=>{};});
  await page.evaluate(()=>{const s=FSCApp.state,p=s.working.profile;for(let i=0;i<20;i++){const c=p.cameras[i]||JSON.parse(JSON.stringify(p.cameras[0]));c.id=p.cameras[i]?c.id:crypto.randomUUID();c.viewId=i+1;if(!p.cameras[i]){p.cameras.push(c);p.categories[0].cameraIds.push(c.id);}}p.cameras[20]=JSON.parse(JSON.stringify(p.cameras[0]));const c=p.cameras[20];c.id=crypto.randomUUID();c.name='Extra camera';c.viewId=0;p.categories[0].cameraIds.push(c.id);s.selection.cameraId=c.id;});
  await page.locator('#mc-nav-cameras').click();await page.locator('#mc-camera-binding').click();
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras[20].viewId),21);
  await page.locator('#mc-dialog-close').click();
  }
  await page.goto((process.argv[2]?url.replace(/main\.html$/,'mini.html'):url)+'?surface=mini');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  // Large saved airport fixture; capture UI sends without depending on the demo simulator.
  await page.evaluate(()=>{
    const s=FSCApp.state,p=s.working.profile,cat=p.categories[0],base=p.cameras[0];cat.name='Apron';cat.airportIcao='KDAB';cat.cameraIds=[];p.cameras=[];
    for(let i=0;i<100;i++){const c=JSON.parse(JSON.stringify(base));c.id=crypto.randomUUID();c.name='Parking '+(i+1);c.mode='airport';c.airportIcao='KDAB';c.viewId=100+i;p.cameras.push(c);cat.cameraIds.push(c.id);}cat.defaultCameraId=cat.cameraIds[0];p.categories=[cat];p.defaultCameraId=cat.cameraIds[0];s.selection.cameraId=cat.cameraIds[0];s.active.cameraId=cat.cameraIds[0];s.ownership.state='Active';s.miniCategory=cat.id;s.airportExpanded=true;
    s.working.settings.ui.miniHover=false;
    s.airports.autoSwitch=false;s.airports.positionKnown=true;window.calls=[];
    FSCApp.bridge.request=async(a,p)=>{calls.push({a,p});if(a==='SetAirportAuto')s.airports.autoSwitch=p.enabled;return{status:'Completed'};};
    FSCDemo.setOwnership('Active');
    document.getElementById('mc-mini-pin').click();
  });
  await page.locator('#mc-mouse-look-auto').click();assert.ok(await page.evaluate(()=>calls.some(c=>c.a==='SetAirportAuto'&&c.p.enabled)));
  assert.ok(await page.locator('#mc-mouse-look-save').isVisible());
  await page.locator('#mc-mouse-look-aircraft').click();
  await page.waitForFunction(()=>document.querySelector('.mc-tracking-shade .mc-dialog').style.top);
  const trackingBox=await page.locator('.mc-tracking-shade .mc-dialog').boundingBox();
  const dockBox=await page.locator('#mc-mini-panel').boundingBox();
  assert.ok(trackingBox.width<=360 && trackingBox.height<450,'tracking menu stays compact');
  assert.ok(trackingBox.y+trackingBox.height<=dockBox.y,'menu sits above dock');
  assert.equal(await page.locator('#mc-dialog-shade').evaluate(e=>getComputedStyle(e).backgroundColor),'rgba(0, 0, 0, 0)');
  assert.equal(await page.locator('#mc-dialog-shade').evaluate(e=>getComputedStyle(e).pointerEvents),'none');
  await page.screenshot({path:'build/review/airport-tracking-menu.png'});
  await page.getByRole('button',{name:'Track heading and pitch',exact:true}).click();
  assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.host.globalAircraftTracking),'both','Tracking choice changes the global setting');
  const size=await page.locator('#mc-dock-favorites').evaluate(e=>({client:e.clientHeight,scroll:e.scrollHeight,count:e.querySelectorAll('button').length}));
  assert.equal(size.count,100);assert.ok(size.scroll>size.client);assert.ok(size.client<=90);
  assert.ok((await page.locator('#mc-mini-panel').boundingBox()).height<=122);
  await page.locator('#mc-dock-favorites').evaluate(e=>e.scrollTop=e.scrollHeight);
  assert.ok(await page.locator('#mc-dock-favorites').evaluate(e=>e.scrollTop>0));
  await page.screenshot({path:'build/review/airport-controls-mini.png'});
  await page.locator('#mc-mini-panel').screenshot({path:'build/review/airport-controls-mini-detail.png'});
  await page.setViewportSize({width:750,height:900});
  await page.screenshot({path:'build/review/airport-controls-mini-narrow.png'});
  const dock=await page.locator('#mc-mini-panel').boundingBox();
  for(const id of ['mc-mouse-look-auto','mc-mouse-look-save','mc-dock-favorites']){
    const box=await page.locator('#'+id).boundingBox();assert.ok(box.x>=dock.x&&box.x+box.width<=dock.x+dock.width+1&&box.y+box.height<=dock.y+dock.height+1,id+' stays inside the compact panel '+JSON.stringify({box,dock}));
  }
  await page.locator('#mc-dock-favorites').evaluate(e=>e.scrollTop=e.scrollHeight);
  const last=await page.locator('#mc-dock-favorites button').last().boundingBox(),list=await page.locator('#mc-dock-favorites').boundingBox();
  assert.ok(last.y>=list.y&&last.y+last.height<=list.y+list.height+1,'Last camera remains reachable after resizing');
  await page.screenshot({path:'build/review/airport-controls-mini-narrow.png'});
  assert.deepEqual(errors,[]);console.log((process.env.FSCS_MINI_ONLY?'Mini controls UI: compact tracking dialog, auto switch, Save and 100-camera scroll passed: ':'Flight controls UI passed: ')+(process.argv[2]||'source'));
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
