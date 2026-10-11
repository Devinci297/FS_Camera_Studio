const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const errors=[],page=await browser.newPage({viewport:{width:1200,height:1000}});
  page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const tip=page.locator('#mc-control-tooltip');
  await page.locator('#mc-camera-icon').evaluate(el=>el.setAttribute('aria-describedby','existing-help'));
  await page.locator('#mc-camera-icon').hover();await page.mouse.move(1,1);
  assert.equal(await page.locator('#mc-camera-icon').getAttribute('aria-describedby'),'existing-help');
  await page.locator('#mc-camera-icon').hover();
  await tip.waitFor();assert.match(await tip.textContent(),/Choose an icon/);
  assert.equal(await tip.evaluate(el=>getComputedStyle(el).pointerEvents),'none');
  await page.mouse.move(1,1);await tip.waitFor({state:'hidden'});
  await page.locator('#mc-camera-name').focus();await tip.waitFor();
  assert.match(await tip.textContent(),/Rename/);
  assert.match(await page.locator('#mc-camera-name').getAttribute('aria-describedby'),/mc-control-tooltip/);
  await page.keyboard.press('Escape');await tip.waitFor({state:'hidden'});
  for(const mode of ['icons','labels','both']) {
   await page.locator('#mc-nav-settings').click();
   assert.equal(await page.locator('#mc-name-display-label').textContent(),'Category display');
   await page.locator('#mc-setting-name-display [data-value="'+mode+'"]').click();
   await page.locator('#mc-nav-cameras').click();
   if((await page.locator('#mc-toggle-categories').textContent()).includes('Expand'))await page.locator('#mc-toggle-categories').click();
   assert.ok(await page.locator('.mc-camera-select .mc-entity-label').first().isVisible(),'Main camera names remain visible: '+mode);
   assert.ok(await page.locator('.mc-camera-select .mc-entity-name>svg').first().isVisible(),'Main camera icons remain visible: '+mode);
   assert.equal(await page.locator('.mc-category-head .mc-entity-label').first().isVisible(),mode!=='icons');
   assert.equal(await page.locator('.mc-category-head .mc-entity-name>svg').first().isVisible(),mode!=='labels');
  }
  await page.locator('#mc-nav-settings').click();
  for(const opacity of [15,100]) {
   await page.locator('#mc-setting-mini-opacity').evaluate((el,value)=>{el.value=value;el.dispatchEvent(new Event('input',{bubbles:true}));},opacity);
   const bg=await page.locator('#mc-mini-panel').evaluate(el=>el.style.background);
   assert.ok(bg.includes(opacity===15?'0.15':'rgb(14, 25, 35)'),bg);
  }
  await page.goto(url.replace(/main\.html$/,'mini.html')+'?surface=mini');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.waitForTimeout(600);
  await page.evaluate(()=>{
   const s=FSCApp.state,p=s.working.profile,cat=JSON.parse(JSON.stringify(p.categories[0]));
   cat.id=crypto.randomUUID();cat.name='Airport gates';cat.airportIcao='KAAA';cat.cameraIds=[];
   const c=JSON.parse(JSON.stringify(p.cameras[0]));c.id=crypto.randomUUID();c.name='Gate A1';c.mode='airport';c.airportIcao='KAAA';p.cameras.push(c);cat.cameraIds.push(c.id);p.categories.push(cat);
   s.caps.modes.airport={available:true};s.working.settings.ui.miniHover=false;s.working.settings.ui.miniOpacity=.15;
   FSCApp.bridge.request=async()=>({status:'Completed'});FSCApp.bridge.send=()=>{};
  });
  for(const airport of [false,true])for(const mode of ['icons','labels','both']) {
   await page.evaluate(({airport,mode})=>{
    const s=FSCApp.state;s.miniCategory=s.working.profile.categories[airport?s.working.profile.categories.length-1:0].id;
    document.querySelector('#mc-setting-name-display [data-value="'+mode+'"]').click();FSCDemo.setOwnership('Observing');
   },{airport,mode});
   const dock=await page.locator('#mc-mini-panel').boundingBox();assert.ok(dock.x<=1&&dock.width>=1199&&dock.height<=85,'Full-width compact mini panel');
   assert.ok(await page.locator('#mc-dock-favorites .mc-entity-label').first().isVisible(),'Mini camera names remain visible: '+airport+'/'+mode);
   assert.ok(await page.locator('#mc-dock-favorites .mc-entity-name>svg').first().isVisible(),'Mini camera icons remain visible: '+airport+'/'+mode);
   assert.equal(await page.locator('#mc-dock-categories .mc-entity-label').first().isVisible(),mode!=='icons');
   assert.equal(await page.locator('#mc-dock-categories .mc-entity-name>svg').first().isVisible(),mode!=='labels');
   await page.locator('#mc-dock-favorites button').first().hover();await tip.waitFor();
   assert.match(await tip.textContent(),/Right-click for camera actions/);
   const rect=await tip.boundingBox();assert.ok(rect.x>=0&&rect.y>=0&&rect.x+rect.width<=1201&&rect.y+rect.height<=1001,'Tooltip stays in viewport');
  }
  await page.locator('#mc-mini-panel').screenshot({path:'build/review/mini-compact-full-width.png'});
  assert.match(await page.locator('#mc-mini-panel').evaluate(el=>getComputedStyle(el).backgroundColor),/0\.15/);
  assert.equal(await page.locator('#mc-dock-favorites button').first().evaluate(el=>getComputedStyle(el).opacity),'1');
  assert.equal(await page.locator('#mc-mini-panel').evaluate(el=>getComputedStyle(el).backdropFilter),'none');
  assert.deepEqual(errors,[]);
  console.log('Main and mini tooltips, keyboard focus, category-only display modes, and background-only opacity passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
