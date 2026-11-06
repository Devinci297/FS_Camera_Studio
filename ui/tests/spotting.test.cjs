const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Spotting: the Traffic page (sorted list, filters, view buttons and their gating, state header, Stop, Auto-spot,
// empty state), the Settings section, the binding rows, and the mini panel's Auto-spot / Next target controls,
// against the demo module's simulated traffic. FSCDemo.traffic({time}) freezes the traffic paths.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url=surface=>'file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface='+surface;
  const open=async(surface,viewport)=>{
   const page=await browser.newPage({viewport:viewport||{width:1400,height:1000}}),errors=[];
   page.on('pageerror',e=>errors.push(e.message));
   await page.goto(url(surface));
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>{window.calls=[];const original=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=(a,p)=>{calls.push({a,p});return original(a,p);};});
   return {page,errors};
  };
  const sent=(page,action,payload)=>page.waitForFunction(([a,p])=>calls.some(c=>c.a===a&&Object.keys(p).every(k=>c.p[k]===p[k])),[action,payload||{}]);
  const spot=page=>page.evaluate(()=>FSCApp.state.spotting||{state:'off'});
  const freeze=(page,time)=>page.evaluate(t=>FSCDemo.traffic({time:t}),time);

  const {page,errors}=await open('main');
  const traffic=await freeze(page,100);
  assert.ok(traffic.aircraft.length>=6,'the demo simulates nearby traffic');
  await page.waitForFunction(n=>FSCApp.state.traffic.aircraft.length===n,traffic.aircraft.length);
  assert.ok(await page.evaluate(()=>Array.isArray(FSCApp.state.traffic.aircraft)),'Snapshot and TrafficChanged fill the traffic list');

  // Traffic page: a tab like the others, nearest aircraft first, label, status chip and figures.
  await page.locator('#mc-nav-traffic').click();
  assert.ok(await page.locator('#mc-page-traffic').isVisible());
  assert.equal(await page.locator('#mc-nav-traffic').getAttribute('aria-selected'),'true');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/Not spotting/);
  assert.equal(await page.locator('#mc-spot-stop').isDisabled(),true);
  const rows=page.locator('#mc-traffic-list .mc-traffic-row');
  await page.waitForFunction(n=>document.querySelectorAll('#mc-traffic-list .mc-traffic-row').length===n,traffic.aircraft.length);
  const order=await rows.evaluateAll(list=>list.map(r=>Number(r.dataset.objectId)));
  const sorted=traffic.aircraft.slice().sort((a,b)=>a.distanceNm-b.distanceNm).map(a=>a.objectId);
  assert.deepEqual(order,sorted,'rows are sorted by distance');
  assert.match(await page.locator('#mc-traffic-count').innerText(),new RegExp(traffic.aircraft.length+' aircraft within 20 NM'));
  const first=rows.first();
  assert.equal(await first.locator('.mc-traffic-name strong').innerText(),'Lufthansa 441');
  assert.match(await first.locator('.mc-traffic-name small').innerText(),/A320 · D-AINB/);
  assert.equal((await first.locator('.mc-traffic-status').innerText()).toLowerCase(),'parked');
  assert.match(await first.locator('.mc-traffic-figures').innerText(),/^0\.1 NM, 1,417 ft, 0 kt$/);
  const gaRow=rows.filter({hasText:'N172DM'});
  assert.match(await gaRow.locator('.mc-traffic-name strong').innerText(),/^N172DM$/,'the ATC id labels aircraft without an airline flight');
  assert.match(await rows.filter({hasText:'United 15'}).locator('.mc-traffic-figures').innerText(),/35,000 ft, 480 kt/);

  // Filters: status, airline or callsign text, maximum distance.
  const shown=()=>rows.evaluateAll(list=>list.map(r=>({id:Number(r.dataset.objectId),status:r.dataset.status,ground:r.dataset.onGround==='true'})));
  const expect=keep=>traffic.aircraft.filter(keep).length;
  await page.locator('#mc-traffic-filter [data-value=ground]').click();
  let list=await shown();
  assert.equal(list.length,expect(a=>a.onGround));assert.ok(list.length>0&&list.every(r=>r.ground));
  assert.equal(await page.locator('#mc-traffic-filter [data-value=ground]').getAttribute('aria-pressed'),'true');
  await page.locator('#mc-traffic-filter [data-value=arriving]').click();
  list=await shown();
  assert.equal(list.length,expect(a=>a.status==='arriving'||a.status==='landing'));assert.ok(list.length>0&&list.every(r=>r.status==='arriving'||r.status==='landing'));
  await page.locator('#mc-traffic-filter [data-value=departing]').click();
  list=await shown();
  assert.equal(list.length,expect(a=>a.status==='departing'||a.status==='takeoff'));assert.ok(list.length>0&&list.every(r=>r.status==='departing'||r.status==='takeoff'));
  assert.match(await page.locator('#mc-traffic-count').innerText(),new RegExp(list.length+' of '+traffic.aircraft.length+' aircraft'));
  await page.locator('#mc-traffic-filter [data-value=all]').click();
  assert.equal((await shown()).length,traffic.aircraft.length);
  for(const [query,label] of [['easyjet','easyJet 2914'],['G-EZ','easyJet 2914'],['airways 710','British Airways 710'],['n456','N456AB']]){
   await page.locator('#mc-traffic-search').fill(query);
   assert.equal(await rows.count(),1,query);
   assert.equal(await rows.first().locator('.mc-traffic-name strong').innerText(),label);
  }
  await page.locator('#mc-traffic-search').fill('zzzz');
  assert.equal(await rows.count(),0);
  assert.ok(await page.locator('#mc-traffic-none').isVisible(),'no-match note');
  assert.ok(await page.locator('#mc-traffic-empty').isHidden(),'the no-traffic text is only for an empty list');
  await page.locator('#mc-traffic-search').fill('');
  await page.locator('#mc-traffic-distance').selectOption('5');
  list=await shown();
  assert.equal(list.length,expect(a=>a.distanceNm<=5));assert.ok(list.length<traffic.aircraft.length);
  await page.locator('#mc-traffic-distance').selectOption('0');
  assert.equal(await rows.count(),traffic.aircraft.length);

  // View buttons: Tower and Runway need a loaded airport set; Track needs an airport or free view on screen.
  const row=label=>rows.filter({has:page.locator('.mc-traffic-name strong',{hasText:label})});
  const view=(label,name)=>row(label).locator('button[data-view='+name+']');
  assert.equal(await view('easyJet 2914','tower').isDisabled(),true);
  assert.equal(await view('easyJet 2914','runway').isDisabled(),true);
  assert.match(await view('easyJet 2914','tower').getAttribute('title'),/airport cameras/);
  assert.equal(await view('easyJet 2914','track').isDisabled(),true);
  assert.match(await view('easyJet 2914','track').getAttribute('title'),/airport or free camera/);
  assert.equal(await view('easyJet 2914','chase').isEnabled(),true);
  assert.equal(await view('easyJet 2914','flyby').isEnabled(),true);
  assert.equal(await view('easyJet 2914','chase').getAttribute('aria-label'),'Chase view of easyJet 2914');
  // The module rejects an airport view without an airport set, with a reason.
  const rejected=await page.evaluate(()=>FSCApp.bridge.request('SpotTarget',{objectId:104,view:'tower'}));
  assert.equal(rejected.status,'Rejected');assert.match(rejected.reason,/airport cameras/);

  await view('easyJet 2914','chase').click();
  await sent(page,'SpotTarget',{objectId:104,view:'chase'});
  await page.waitForFunction(()=>FSCApp.state.spotting&&FSCApp.state.spotting.state==='tracking'&&FSCApp.state.spotting.targetId===104);
  await page.waitForFunction(()=>FSCApp.state.ownership.state==='Active');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/^Spotting easyJet 2914 · A319 · Chase$/);
  assert.equal(await page.locator('#mc-spot-state').getAttribute('data-state'),'tracking');
  assert.equal(await row('easyJet 2914').getAttribute('data-tracking'),'true');
  assert.equal(await view('easyJet 2914','chase').getAttribute('aria-pressed'),'true');
  assert.equal(await page.locator('#mc-spot-stop').isEnabled(),true);
  assert.equal(await view('easyJet 2914','track').isDisabled(),true,'a chase view cannot be re-aimed');
  await view('British Airways 710','flyby').click();
  await sent(page,'SpotTarget',{objectId:106,view:'flyby'});
  await page.waitForFunction(()=>FSCApp.state.spotting.targetId===106&&FSCApp.state.spotting.view==='flyby');
  assert.equal(await row('easyJet 2914').getAttribute('data-tracking'),'false');

  // Near an airport: tower and runway views, then Track keeps that airport view and aims it.
  await page.evaluate(()=>FSCDemo.arriveAtAirport(true));
  await page.waitForFunction(()=>FSCApp.state.airports.loaded==='KDEM');
  await page.waitForFunction(()=>!document.querySelector('#mc-traffic-list button[data-view=tower]').disabled);
  await view('British Airways 710','tower').click();
  await sent(page,'SpotTarget',{objectId:106,view:'tower'});
  await page.waitForFunction(()=>FSCApp.state.spotting.view==='tower');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/British Airways 710 · A321 · Tower/);
  await page.waitForFunction(()=>!document.querySelector('#mc-traffic-list button[data-view=track]').disabled);
  await view('easyJet 2914','runway').click();
  await sent(page,'SpotTarget',{objectId:104,view:'runway'});
  await page.waitForFunction(()=>FSCApp.state.spotting.view==='runway'&&FSCApp.state.spotting.targetId===104);
  await view('United 15','track').click();
  await sent(page,'SpotTarget',{objectId:108,view:'track'});
  await page.waitForFunction(()=>FSCApp.state.spotting.view==='track'&&FSCApp.state.spotting.targetId===108);

  // A target that leaves the traffic radius is reported lost.
  await freeze(page,329);
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='lost');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/^Lost United 15 · B789: .*radius/);
  await freeze(page,100);

  // Stop returns to the view shown before spotting began.
  await page.locator('#mc-spot-stop').click();
  await sent(page,'StopSpotting');
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/Not spotting/);
  assert.equal(await page.locator('#mc-spot-stop').isDisabled(),true);
  assert.equal(await rows.evaluateAll(list=>list.filter(r=>r.dataset.tracking==='true').length),0);

  // Auto-spot: on picks an arriving aircraft, off sends enabled:false; with no traffic it waits.
  await page.locator('#mc-spot-auto').click();
  await sent(page,'SetAutoSpot',{enabled:true});
  await page.waitForFunction(()=>FSCApp.state.spotting.autoSpot&&FSCApp.state.spotting.state==='tracking');
  assert.equal(await page.locator('#mc-spot-auto').getAttribute('aria-pressed'),'true');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/· Auto-spot$/);
  const picked=(await spot(page)).targetId;
  assert.ok(['arriving','landing','takeoff','departing'].includes(traffic.aircraft.find(a=>a.objectId===picked).status),'Auto-spot follows an arrival or departure');
  await page.locator('#mc-spot-auto').click();
  await sent(page,'SetAutoSpot',{enabled:false});
  await page.waitForFunction(()=>!FSCApp.state.spotting.autoSpot);
  assert.equal(await page.locator('#mc-spot-auto').getAttribute('aria-pressed'),'false');
  await page.locator('#mc-spot-stop').click();
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');

  // Empty state: plain text about AI, VATSIM/IVAO and MSFS multiplayer traffic.
  await page.evaluate(()=>FSCDemo.traffic({enabled:false}));
  await page.waitForFunction(()=>FSCApp.state.traffic.aircraft.length===0);
  assert.equal(await rows.count(),0);
  assert.ok(await page.locator('#mc-traffic-empty').isVisible());
  const emptyText=await page.locator('#mc-traffic-empty').innerText();
  assert.match(emptyText,/No traffic nearby/);
  assert.match(emptyText,/VATSIM and IVAO traffic appears when your network client injects it/);
  assert.match(emptyText,/multiplayer aircraft are not visible to add-ons/);
  assert.ok(await page.locator('#mc-traffic-none').isHidden());
  await page.locator('#mc-spot-auto').click();
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='waiting');
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/Auto-spot is waiting/);
  await page.locator('#mc-spot-stop').click();
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off'&&!FSCApp.state.spotting.autoSpot);
  await page.evaluate(()=>FSCDemo.traffic({enabled:true}));
  await page.waitForFunction(n=>FSCApp.state.traffic.aircraft.length===n,traffic.aircraft.length);

  // Release and Panic stop spotting.
  await view('Lufthansa 441','chase').click();
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='tracking');
  await page.waitForFunction(()=>FSCApp.state.ownership.state==='Active');
  await page.evaluate(()=>FSCApp.bridge.request('ReleaseCamera',{}));
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');
  assert.match((await spot(page)).reason,/released/);
  await view('Lufthansa 441','chase').click();
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='tracking');
  await page.evaluate(()=>{window.panic=null;FSCApp.bridge.on('event',e=>{if(e.name==='PanicReleased')window.panic=e.payload;});});
  await page.evaluate(()=>FSCApp.bridge.request('TestBinding',{binding:{action:'panicRelease',viewId:0}}));
  await page.waitForFunction(()=>window.panic&&FSCApp.state.spotting.state==='off');
  assert.equal(await page.evaluate(()=>window.panic.spottingStopped),true);

  // Bindings: their own section with friendly labels and the demo defaults; pressing them drives spotting.
  await page.locator('#mc-nav-bindings').click();
  await page.locator('#mc-binding-search').fill('spot');
  for(const [action,label,key] of [['spotNext','Next spotting target','T'],['spotPrevious','Previous spotting target','Y'],['spotReturn','Return to own aircraft','O'],['toggleAutoSpot','Auto-spot on/off','J']]){
   const binding=page.locator('[data-binding='+action+']');
   assert.equal(await binding.count(),1,action);
   assert.match(await binding.innerText(),new RegExp(label.replace('/','\\/')));
   assert.equal(await binding.locator('[data-slot=primary] .mc-binding-input').innerText(),'Ctrl + Alt + '+key);
  }
  assert.ok((await page.locator('.mc-binding-section summary').allInnerTexts()).some(t=>/^Spotting - other aircraft \(4\)/.test(t)));
  await page.locator('#mc-binding-search').fill('');
  const press=action=>page.evaluate(a=>FSCApp.bridge.request('TestBinding',{binding:{action:a,viewId:0}}),action);
  await press('spotNext');
  await page.waitForFunction(id=>FSCApp.state.spotting.state==='tracking'&&FSCApp.state.spotting.targetId===id,sorted[0]);
  await press('spotNext');
  await page.waitForFunction(id=>FSCApp.state.spotting.targetId===id,sorted[1]);
  await press('spotPrevious');
  await page.waitForFunction(id=>FSCApp.state.spotting.targetId===id,sorted[0]);
  assert.equal((await spot(page)).view,'chase');
  await press('toggleAutoSpot');
  await page.waitForFunction(()=>FSCApp.state.spotting.autoSpot);
  await press('toggleAutoSpot');
  await page.waitForFunction(()=>!FSCApp.state.spotting.autoSpot);
  await press('spotReturn');
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');

  // Settings: traffic radius and Auto-spot hold time, clamped, as unsaved host settings.
  await page.locator('#mc-nav-settings').click();
  await page.locator('#mc-settings-link-spotting').click();
  assert.ok(await page.locator('#mc-settings-section-spotting').isVisible());
  assert.equal(await page.locator('.mc-settings-pane:visible').count(),1);
  assert.equal(await page.evaluate(()=>document.getElementById('mc-settings-link-cinematic').nextElementSibling.id),'mc-settings-link-spotting');
  const value=id=>page.locator('#'+id).inputValue();
  assert.equal(await value('mc-setting-spotting-radius'),'20');
  assert.equal(await value('mc-setting-autospot-hold'),'30');
  const host=()=>page.evaluate(()=>FSCApp.state.working.settings.host);
  const set=async(id,v)=>{await page.locator('#'+id).fill(v);await page.locator('#'+id).press('Tab');};
  await set('mc-setting-spotting-radius','150');
  assert.equal((await host()).spottingRadiusNm,100,'clamped to 100 NM');
  assert.equal(await value('mc-setting-spotting-radius'),'100');
  await set('mc-setting-spotting-radius','0');
  assert.equal((await host()).spottingRadiusNm,1,'clamped to 1 NM');
  await set('mc-setting-autospot-hold','5');
  assert.equal((await host()).autoSpotHoldSeconds,10,'clamped to 10 s');
  await set('mc-setting-autospot-hold','400');
  assert.equal((await host()).autoSpotHoldSeconds,300,'clamped to 300 s');
  await set('mc-setting-autospot-hold','45');
  assert.equal((await host()).autoSpotHoldSeconds,45);
  await set('mc-setting-spotting-radius','5');
  assert.equal((await host()).spottingRadiusNm,5);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.dirty.settings||document.getElementById('mc-save-state').dataset.dirty==='true'),true,'settings changes are unsaved edits');
  await page.waitForFunction(()=>!FSCApp.state.inflight&&!FSCApp.state.queue.length);
  await freeze(page,100);
  await page.waitForFunction(()=>FSCApp.state.traffic.aircraft.every(a=>a.distanceNm<=5));
  await page.locator('#mc-nav-traffic').click();
  assert.match(await page.locator('#mc-traffic-count').innerText(),/within 5 NM/);
  assert.equal(await rows.count(),traffic.aircraft.filter(a=>a.distanceNm<=5).length);
  assert.deepEqual(errors,[]);
  await page.close();

  // Mini panel: Auto-spot and Next target next to Cinematic, a short status line, and a compact dock.
  const mini=await open('mini',{width:1600,height:1000});
  await mini.page.evaluate(()=>document.getElementById('mc-mini-pin').click());
  await freeze(mini.page,100);
  const auto=mini.page.locator('#mc-mouse-look-autospot'),next=mini.page.locator('#mc-mouse-look-spotnext'),status=mini.page.locator('#mc-mouse-look-spotting');
  await next.waitFor();
  assert.equal(await mini.page.evaluate(()=>document.getElementById('mc-mouse-look-cinematic').nextElementSibling.id),'mc-mouse-look-autospot');
  assert.equal(await mini.page.evaluate(()=>document.getElementById('mc-mouse-look-autospot').nextElementSibling.id),'mc-mouse-look-spotnext');
  assert.equal((await next.innerText()).trim(),'Next target');
  assert.ok(await status.isHidden(),'no status line while not spotting');
  await mini.page.waitForFunction(()=>!document.getElementById('mc-mouse-look-spotnext').disabled);
  await next.click();
  await sent(mini.page,'SpotTarget',{objectId:sorted[0],view:'chase'});
  await mini.page.waitForFunction(()=>FSCApp.state.spotting&&FSCApp.state.spotting.state==='tracking');
  await status.waitFor();
  assert.equal(await status.innerText(),'Spotting: Lufthansa 441 · A320');
  await next.click();
  await sent(mini.page,'SpotTarget',{objectId:sorted[1],view:'chase'});
  await mini.page.waitForFunction(id=>FSCApp.state.spotting.targetId===id,sorted[1]);
  await auto.click();
  await sent(mini.page,'SetAutoSpot',{enabled:true});
  await mini.page.waitForFunction(()=>document.getElementById('mc-mouse-look-autospot').getAttribute('aria-pressed')==='true');
  await auto.click();
  await sent(mini.page,'SetAutoSpot',{enabled:false});
  await mini.page.waitForFunction(()=>document.getElementById('mc-mouse-look-autospot').getAttribute('aria-pressed')==='false');
  await mini.page.evaluate(()=>FSCApp.bridge.request('StopSpotting',{}));
  await mini.page.waitForFunction(()=>FSCApp.state.spotting.state==='off');
  await status.waitFor({state:'hidden'});
  // The dock height never changes; from 1300 px the toolbar fits without scrolling (mid widths show icons only).
  for(const width of [1200,1300,1450]){
   await mini.page.setViewportSize({width,height:1000});await mini.page.waitForTimeout(600);
   const geometry=await mini.page.evaluate(()=>{const dock=document.getElementById('mc-mini-panel'),tools=dock.querySelector('.mc-dock-tools');return {height:dock.getBoundingClientRect().height,overflow:tools.scrollWidth>tools.clientWidth+1};});
   assert.ok(geometry.height<=122,'the dock stays compact ('+geometry.height+')');
   if(width>=1300)assert.equal(geometry.overflow,false,'the toolbar keeps one row at '+width);
   assert.equal(await mini.page.getByRole('button',{name:'Next target',exact:true}).count(),1,'icon-only at mid widths, still named');
  }
  await mini.page.evaluate(()=>FSCDemo.traffic({enabled:false}));
  await mini.page.waitForFunction(()=>document.getElementById('mc-mouse-look-spotnext').disabled);
  assert.match(await next.getAttribute('title'),/No traffic nearby/);
  // Condensed mini: both controls move into More, under Spotting.
  await mini.page.setViewportSize({width:1000,height:1000});await mini.page.waitForTimeout(600);
  assert.ok(await next.isHidden());
  await mini.page.locator('#mc-mouse-look-more').click();
  const menu=mini.page.locator('#mc-dialog-body');
  assert.ok((await menu.locator('.mc-section-label').allTextContents()).includes('Spotting'));
  assert.equal(await menu.getByRole('button',{name:'Auto-spot',exact:true}).count(),1);
  assert.equal(await menu.getByRole('button',{name:'Next target',exact:true}).isDisabled(),true);
  await mini.page.keyboard.press('Escape');
  assert.deepEqual(mini.errors,[]);
  console.log('Spotting traffic list, filters, view gating, state header, Stop, Auto-spot, empty state, settings, bindings and mini controls passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
