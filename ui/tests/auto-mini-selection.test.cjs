const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1000,height:800}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>{
   let api;
   Object.defineProperty(window,'FSCBridge',{configurable:true,get(){return api;},set(value){
    api=value;const create=api.create;api.create=function(opts){
     const bridge=create(opts),on=bridge.on,listeners=[];
     bridge.on=function(type,fn){if(type==='event')listeners.push(fn);return on.call(bridge,type,fn);};
     window.testHostEvent=(name,payload)=>listeners.forEach(fn=>fn({name,payload}));return bridge;
    };
   }});
  });
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  for(const surface of ['main','mini']) {
   await page.goto((surface==='mini'?url.replace(/main\.html$/,'mini.html'):url)+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.waitForTimeout(600);
   await page.evaluate(()=>{
    const s=FSCApp.state,p=s.working.profile;
    window.originalSelection=JSON.stringify(s.selection);window.cockpitCategory=p.categories[0].id;
    window.airportGroups=[];
    for(const name of ['Gates','Runways']){
     const g=JSON.parse(JSON.stringify(p.categories[0]));g.id=crypto.randomUUID();g.name=name;g.airportIcao='KAAA';g.cameraIds=[];
     for(let i=0;i<80;i++){const c=JSON.parse(JSON.stringify(p.cameras[0]));c.id=crypto.randomUUID();c.name=name+' '+i;c.mode='airport';c.airportIcao='KAAA';p.cameras.push(c);g.cameraIds.push(c.id);}
     p.categories.push(g);airportGroups.push(g);
    }
    s.working.settings.ui.miniHover=false;s.airports.autoSwitch=false;s.airports.positionKnown=true;s.caps.modes.airport={available:true};
    s.miniCategory=cockpitCategory;s.active={cameraId:p.cameras[0].id,profileId:p.profileId};s.ownership={state:'Active'};
    FSCApp.bridge.request=async()=>({status:'Completed'});FSCApp.bridge.send=()=>{};
    document.querySelector('#mc-setting-name-display [data-value="both"]').click();
    testHostEvent('OwnershipChanged',s.ownership);
   });
   const prefix=surface==='main'?'mc-editor-mouse-look':'mc-mouse-look';
   assert.ok(await page.locator('#'+prefix+'-aircraft').isHidden(),'Cockpit aircraft tracking control hidden');
   await page.locator('#'+prefix+'-aircraft').evaluate(el=>el.click());
   assert.ok(await page.locator('#mc-dialog-shade').isHidden(),'Hidden cockpit aircraft tracking cannot open');
   assert.ok(await page.locator('#'+prefix+'-head').isVisible(),'Cockpit head tracking remains available');
   if(surface==='mini')await page.locator('#mc-mini-pin').focus();
   const active=async(group,index)=>page.evaluate(({group,index})=>testHostEvent('ActiveCameraChanged',{cameraId:airportGroups[group].cameraIds[index],profileId:FSCApp.state.working.profile.profileId}),{group,index});
   const automatic=async(enabled)=>page.evaluate(enabled=>testHostEvent('AirportsChanged',{...FSCApp.state.airports,autoSwitch:enabled}),enabled);
   const check=async(group,index)=>{
    await page.waitForTimeout(90);
    for(const control of ["#mc-editor-mouse-look-auto","#mc-mouse-look-auto"])assert.equal(await page.locator(control).getAttribute("aria-pressed"),"true","Both surfaces keep the same global toggle after camera changes");
    assert.equal(await page.evaluate(()=>JSON.stringify(FSCApp.state.selection)),await page.evaluate(()=>originalSelection),'Auto-follow never edits main selection');
    if(surface==='main'){
     assert.equal(await page.evaluate(()=>FSCApp.state.miniCategory),await page.evaluate(()=>cockpitCategory));return;
    }
    assert.equal(await page.evaluate(()=>FSCApp.state.miniCategory),await page.evaluate(g=>airportGroups[g].id,group));
    const id=await page.evaluate(({group,index})=>airportGroups[group].cameraIds[index],{group,index});
    const selected=page.locator('#mc-dock-favorites [data-favorite="'+id+'"]');
    assert.equal(await selected.getAttribute('aria-pressed'),'true','Automatic camera highlighted');
    assert.equal(await page.locator('#mc-dock-categories [aria-pressed="true"]').getAttribute('aria-label'),group===0?'Gates':'Runways');
    const list=await page.locator('#mc-dock-favorites').boundingBox(),button=await selected.boundingBox();
    assert.ok(button.y>=list.y-1&&button.y+button.height<=list.y+list.height+1,'Automatic camera scrolled into view');
    assert.equal(await page.evaluate(()=>document.activeElement.id),'mc-mini-pin','Auto-follow preserves keyboard focus');
   };
   // First activation can arrive before the auto-state event; both delivery orders must work.
   await active(0,78);await automatic(true);await check(0,78);
   await active(1,75);await check(1,75);
   await page.evaluate(()=>{FSCApp.state.miniCategory=cockpitCategory;testHostEvent('OwnershipChanged',FSCApp.state.ownership);});
   assert.equal(await page.evaluate(()=>FSCApp.state.miniCategory),await page.evaluate(()=>cockpitCategory),'Unchanged auto camera does not override manual browsing');
   await active(0,0);await check(0,0);
   await automatic(false);await active(1,2);
   assert.equal(await page.evaluate(()=>FSCApp.state.miniCategory),await page.evaluate(s=>s==='mini'?airportGroups[0].id:cockpitCategory,surface),'Auto-off preserves browsing');
   await automatic(true);await check(1,2);
  }
  assert.deepEqual(errors,[]);console.log('Automatic mini category/camera highlight, scrolling, event order, focus/manual browsing and cockpit tracking guards passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
