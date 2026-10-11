const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1200,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  for(const surface of ['main','mini']) {
   await page.goto((surface==='mini'?url.replace(/main\.html$/,'mini.html'):url)+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>{
    const s=FSCApp.state,p=s.working.profile,cat=JSON.parse(JSON.stringify(p.categories[0]));
    cat.id=crypto.randomUUID();cat.name='Gates';cat.airportIcao='KAAA';cat.cameraIds=[];
    for(let i=0;i<50;i++){const c=JSON.parse(JSON.stringify(p.cameras[0]));c.id=crypto.randomUUID();c.name='Gate A'+(i+1);c.mode='airport';c.airportIcao='KAAA';p.cameras.push(c);cat.cameraIds.push(c.id);}
    p.categories.push(cat);s.airports.positionKnown=true;s.airports.autoSwitch=false;s.airportExpanded=true;s.caps.modes.airport={available:true};
    s.working.settings.ui.nameDisplay='icons';s.working.settings.ui.miniHover=false;
    window.calls=[];FSCApp.bridge.request=async(a,p)=>{calls.push({a,p});return{status:'Completed'};};FSCApp.bridge.send=()=>{};
    FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();
   });
   const toggle=page.locator(surface==='main'?'#mc-editor-mouse-look-auto':'#mc-mouse-look-auto');
   assert.ok(await toggle.isVisible(),'Auto switch visible from cockpit');
   assert.ok(await toggle.isEnabled(),'Auto switch enabled before an airport view is active');
   await toggle.click();assert.ok(await page.evaluate(()=>calls.some(c=>c.a==='SetAirportAuto'&&c.p.enabled)));
   if(surface==='main') {
    for(const tab of ['bindings','profiles','settings']){await page.locator('#mc-nav-'+tab).click();assert.ok(await toggle.isVisible(),'Global control survives '+tab);}
    await page.locator('#mc-setting-name-display [data-value="icons"]').click();
    await page.locator('#mc-nav-cameras').click();
    await page.screenshot({path:'build/review/global-airport-main.png'});
   } else {
    await page.evaluate(()=>{const s=FSCApp.state;s.miniCategory=s.working.profile.categories.at(-1).id;FSCDemo.setOwnership('Observing');document.getElementById('mc-sim').dataset.nameDisplay='icons';});
    assert.ok(await page.locator('#mc-dock-categories .mc-entity-label').first().isHidden());
    await page.locator('#mc-dock-favorites .mc-airport-camera-name .mc-entity-label').first().waitFor();assert.ok(await page.locator('#mc-dock-favorites .mc-entity-label').first().isVisible(),'Airport camera keeps name in icon mode');
    for(const width of [1920,1200,750]) {
     await page.setViewportSize({width,height:1000});
     const dock=await page.locator('#mc-mini-panel').boundingBox();
     assert.ok(dock.width>=width-34&&dock.height<=122,'Wide but compact dock');
     assert.ok(Math.abs(dock.x-(width-dock.width)/2)<2,'Centered dock');
     await page.locator('#mc-mini-panel').screenshot({path:'build/review/global-airport-mini-'+width+'.png'});
    }
   }
  }
  assert.deepEqual(errors,[]);console.log('Global auto visibility, cockpit activation, airport labels and wide dock passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
