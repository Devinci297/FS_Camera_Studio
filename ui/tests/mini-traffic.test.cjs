const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Mini panel Traffic tab: a category-grid entry with the aircraft count, the nearby-aircraft pane (nearest first,
// in-place updates), view choice and its gating, SpotTarget / re-aim / Stop, leaving the tab, the empty state,
// and the no-profile case where Traffic is the only tab. Runs against the demo module's simulated traffic.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=mini';
  const open=async(options)=>{
   const o=options||{},page=await browser.newPage({viewport:{width:o.width||1600,height:1000}}),errors=[];
   page.on('pageerror',e=>errors.push(e.message));
   if(o.noProfile)await page.addInitScript(()=>{
    // The module sends working.profile = null when no camera profile is open for the aircraft.
    let real;
    Object.defineProperty(window,'FSCBridge',{configurable:true,get:()=>real,set:value=>{real=Object.assign({},value,{create:opts=>{
     const bridge=value.create(opts),on=bridge.on.bind(bridge);
     bridge.on=(kind,handler)=>on(kind,message=>{if(kind==='event'&&message&&message.name==='Snapshot'&&message.payload&&message.payload.working){message.payload.working.profile=null;message.payload.capabilities=Object.assign({},message.payload.capabilities,{canActivate:false,blockReason:'No camera profile is open for this aircraft.'});}handler(message);});
     return bridge;
    }});}});
   });
   await page.goto(url);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>{window.calls=[];const original=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=(a,p)=>{calls.push({a,p});return original(a,p);};});
   await page.evaluate(()=>document.getElementById('mc-mini-pin').click());
   return {page,errors};
  };
  const sent=(page,action,payload)=>page.waitForFunction(([a,p])=>calls.some(c=>c.a===a&&Object.keys(p).every(k=>c.p[k]===p[k])),[action,payload||{}]);
  const freeze=(page,time)=>page.evaluate(t=>FSCDemo.traffic({time:t}),time);

  const {page,errors}=await open();
  const traffic=await freeze(page,100),sorted=traffic.aircraft.slice().sort((a,b)=>a.distanceNm-b.distanceNm);
  const tab=page.locator('#mc-dock-traffic-tab');

  // The tab sits in the category grid with the aircraft count; camera categories stay the default.
  assert.ok(await tab.isVisible(),'Traffic is in the mini category grid');
  assert.ok(await page.evaluate(()=>Boolean(document.getElementById('mc-dock-traffic-tab').closest('#mc-dock-categories'))));
  await page.waitForFunction(n=>document.getElementById('mc-dock-traffic-count').textContent===String(n),traffic.aircraft.length);
  assert.equal(await tab.getAttribute('aria-pressed'),'false');
  assert.equal(await page.locator('#mc-dock-traffic').count(),0,'camera views show until Traffic is chosen');

  // Open it: nearest aircraft first with label, distance and status.
  await tab.click();
  assert.equal(await tab.getAttribute('aria-pressed'),'true');
  const rows=page.locator('#mc-dock-traffic .mc-dock-traffic-aircraft');
  await page.waitForFunction(n=>document.querySelectorAll('#mc-dock-traffic .mc-dock-traffic-aircraft').length===n,sorted.length);
  assert.deepEqual(await rows.evaluateAll(list=>list.map(b=>Number(b.dataset.objectId))),sorted.map(a=>a.objectId),'nearest first');
  assert.equal(await rows.first().locator('strong').innerText(),'Lufthansa 441');
  assert.equal(await rows.first().locator('small').innerText(),'0.1 NM');
  assert.equal(await rows.first().locator('.mc-dock-traffic-status').innerText(),'Parked');
  assert.equal(await rows.first().getAttribute('aria-label'),'Lufthansa 441, 0.1 NM, Parked');
  // Callsign, distance and status are separated by margins (Coherent ignores flex gap), so they never run together in the sim.
  const spacing=await rows.first().evaluate(b=>{const r=e=>e.getBoundingClientRect(),[name,nm,chip]=[b.querySelector('strong'),b.querySelector('small'),b.querySelector('.mc-dock-traffic-status')];
   return {nameToNm:r(nm).left-r(name).right,nmToChip:r(chip).left-r(nm).right,margins:[getComputedStyle(nm).marginLeft,getComputedStyle(chip).marginLeft],gap:getComputedStyle(b).columnGap,font:parseFloat(getComputedStyle(name).fontSize)};});
  assert.ok(spacing.nameToNm>=8&&spacing.nmToChip>=6,'callsign, distance and status are spaced apart: '+JSON.stringify(spacing));
  assert.deepEqual(spacing.margins,['9px','7px'],'spacing comes from margins');
  assert.ok(spacing.font>=13,'callsigns are at least 13px');
  // The view choice, filters and search sit in a bar outside the scrolling list.
  assert.equal(await page.evaluate(()=>Boolean(document.getElementById('mc-dock-traffic-bar').closest('#mc-dock-favorites'))),false,'bar is not inside the scrolling list');
  assert.equal(await page.locator('#mc-dock-traffic-bar .mc-dock-traffic-views').count(),1);
  assert.equal(await page.locator('.mc-dock-traffic-stop').isHidden(),true,'Stop only while spotting');

  // Views: Chase is the default; Tower and Runway need airport cameras or known runway/tower data, Track an airport or free view.
  const view=v=>page.locator('.mc-dock-traffic-views [data-view='+v+']');
  assert.equal(await view('chase').getAttribute('aria-pressed'),'true');
  for(const v of ['tower','runway','track'])assert.equal(await view(v).isDisabled(),true,v+' is gated');
  assert.match(await view('tower').getAttribute('title'),/airport cameras|tower position is known/);
  assert.equal(await view('flyby').isDisabled(),false);

  // Rows update in place at the 1 Hz traffic rate, so a pointer or focus on them survives.
  const marker=await rows.first().evaluate(b=>{b.dataset.marker='kept';return b.dataset.objectId;});
  await freeze(page,null);await page.waitForTimeout(1300);await freeze(page,100);
  assert.equal(await page.locator('#mc-dock-traffic .mc-dock-traffic-aircraft[data-marker=kept]').getAttribute('data-object-id'),marker);

  // Pick an aircraft: SpotTarget in the chosen view, the row shows as followed and Stop appears.
  await rows.nth(1).click();
  await sent(page,'SpotTarget',{objectId:sorted[1].objectId,view:'chase'});
  await page.waitForFunction(id=>FSCApp.state.spotting&&FSCApp.state.spotting.state==='tracking'&&FSCApp.state.spotting.targetId===id,sorted[1].objectId);
  await page.waitForFunction(()=>document.querySelectorAll('.mc-dock-traffic-aircraft')[1].getAttribute('aria-pressed')==='true');
  assert.equal(await page.locator('.mc-dock-traffic-stop').isVisible(),true);

  // Another view while spotting shows the same aircraft in it.
  await view('flyby').click();
  await sent(page,'SpotTarget',{objectId:sorted[1].objectId,view:'flyby'});
  await page.waitForFunction(()=>FSCApp.state.spotting.view==='flyby');
  await page.waitForFunction(()=>document.querySelector('.mc-dock-traffic-views [data-view=flyby]').getAttribute('aria-pressed')==='true');
  assert.equal(await view('chase').getAttribute('aria-pressed'),'false');

  await page.locator('.mc-dock-traffic-stop').click();
  await sent(page,'StopSpotting');
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');
  await page.waitForFunction(()=>document.querySelector('.mc-dock-traffic-stop').hidden);
  // The chosen view is remembered for the next pick.
  assert.equal(await view('flyby').getAttribute('aria-pressed'),'true');
  await rows.first().click();
  await sent(page,'SpotTarget',{objectId:sorted[0].objectId,view:'flyby'});
  await page.evaluate(()=>FSCApp.bridge.request('StopSpotting',{}));
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');

  // Filters: each chip counts its aircraft; picking one narrows the list without moving the bar.
  const chip=f=>page.locator('#mc-dock-traffic-bar [data-filter='+f+']');
  const statuses=()=>rows.evaluateAll(list=>list.map(b=>b.dataset.status));
  assert.equal(await chip('all').getAttribute('aria-pressed'),'true');
  assert.equal(await chip('all').locator('.mc-dock-traffic-filter-count').innerText(),String(sorted.length));
  const expect={departing:a=>a.status==='departing'||a.status==='takeoff',arriving:a=>a.status==='arriving'||a.status==='landing',ground:a=>a.onGround,cruise:a=>a.status==='cruise'};
  for(const f of Object.keys(expect)){
   const n=sorted.filter(expect[f]).length;
   assert.equal(await chip(f).locator('.mc-dock-traffic-filter-count').innerText(),String(n),f+' count');
   await chip(f).click();
   assert.equal(await chip(f).getAttribute('aria-pressed'),'true');
   await page.waitForFunction(n=>document.querySelectorAll('#mc-dock-traffic .mc-dock-traffic-aircraft').length===n,n);
   assert.deepEqual(await rows.evaluateAll(list=>list.map(b=>Number(b.dataset.objectId))),sorted.filter(expect[f]).map(a=>a.objectId),f+' rows');
   if(!n)assert.match(await page.locator('#mc-dock-traffic .mc-note').innerText(),/^No /);
  }
  await chip('all').click();
  await page.waitForFunction(n=>document.querySelectorAll('#mc-dock-traffic .mc-dock-traffic-aircraft').length===n,sorted.length);
  // The bar stays put while the list scrolls under it.
  const barTop=await page.locator('#mc-dock-traffic-bar').evaluate(e=>e.getBoundingClientRect().top);
  await page.evaluate(()=>{const f=document.getElementById('mc-dock-favorites');f.scrollTop=f.scrollHeight;});
  assert.ok(await page.evaluate(()=>document.getElementById('mc-dock-favorites').scrollTop>0),'the aircraft list scrolls');
  assert.equal(await page.locator('#mc-dock-traffic-bar').evaluate(e=>e.getBoundingClientRect().top),barTop,'bar does not scroll');
  assert.ok(await page.locator('#mc-dock-traffic-bar [data-filter=arriving]').isVisible());

  // Search: airline, callsign or type, spaces and dashes ignored; the filter chips count within the search.
  const search=page.locator('#mc-dock-traffic-search');
  await search.fill('a32');
  await page.waitForFunction(()=>document.querySelectorAll('#mc-dock-traffic .mc-dock-traffic-aircraft').length===2);
  assert.deepEqual(await rows.evaluateAll(list=>list.map(b=>Number(b.dataset.objectId))),[101,106],'type search finds the A320 and A321');
  assert.equal(await chip('all').locator('.mc-dock-traffic-filter-count').innerText(),'2');
  await search.fill('hb-jca');
  await page.waitForFunction(()=>document.querySelectorAll('#mc-dock-traffic .mc-dock-traffic-aircraft').length===1);
  assert.equal(await rows.first().locator('strong').innerText(),'Swiss 1084');
  // The list refreshes every second; the search keeps its text and focus.
  await freeze(page,null);await page.waitForTimeout(1300);await freeze(page,100);
  assert.equal(await search.inputValue(),'hb-jca');
  assert.equal(await page.evaluate(()=>document.activeElement&&document.activeElement.id),'mc-dock-traffic-search');
  // Enter follows the first match in the chosen view.
  await search.press('Enter');
  await sent(page,'SpotTarget',{objectId:103,view:'flyby'});
  await page.evaluate(()=>FSCApp.bridge.request('StopSpotting',{}));
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');
  await search.fill('zzz');
  await page.waitForFunction(()=>!document.querySelector('#mc-dock-traffic .mc-dock-traffic-aircraft'));
  assert.match(await page.locator('#mc-dock-traffic .mc-note').innerText(),/No aircraft match "zzz"/);
  await search.press('Escape');
  assert.equal(await search.inputValue(),'');
  await page.waitForFunction(n=>document.querySelectorAll('#mc-dock-traffic .mc-dock-traffic-aircraft').length===n,sorted.length);

  // A camera category leaves the Traffic tab and shows its starred cameras again.
  await page.locator('#mc-dock-categories button[aria-label=Cockpit]').click();
  assert.equal(await page.locator('#mc-dock-traffic').count(),0);
  assert.ok(await page.locator('#mc-dock-traffic-bar').isHidden(),'the traffic bar leaves with the tab');
  assert.equal(await tab.getAttribute('aria-pressed'),'false');
  assert.ok(await page.locator('#mc-dock-favorites [data-favorite]').count()>0);

  // No traffic: an explanation instead of an empty pane, and no count badge.
  await tab.click();
  await page.evaluate(()=>FSCDemo.traffic({enabled:false}));
  await page.waitForFunction(()=>!document.querySelector('.mc-dock-traffic-aircraft'));
  assert.match(await page.locator('#mc-dock-traffic .mc-note').innerText(),/No other aircraft within 20 NM/);
  assert.equal(await page.locator('#mc-dock-traffic-count').isHidden(),true);
  await page.evaluate(()=>FSCDemo.traffic({enabled:true}));

  // The dock keeps its height and the tools row does not overflow at the widths the mini panel supports.
  for(const width of [1000,1300,1600]){
   await page.setViewportSize({width,height:1000});await page.waitForTimeout(500);
   const g=await page.evaluate(()=>{const dock=document.getElementById('mc-mini-panel'),pane=document.getElementById('mc-dock-traffic'),fav=document.getElementById('mc-dock-favorites'),bar=document.getElementById('mc-dock-traffic-bar').getBoundingClientRect(),d=dock.getBoundingClientRect(),first=pane.querySelector('.mc-dock-traffic-aircraft');
    return {dock:d.height,inside:pane.getBoundingClientRect().left>=fav.getBoundingClientRect().left-1&&pane.getBoundingClientRect().right<=fav.getBoundingClientRect().right+1,barInside:bar.right<=d.right+1&&bar.bottom<=d.bottom,listRoom:fav.getBoundingClientRect().height,row:first?first.getBoundingClientRect().height:0};});
   assert.ok(g.dock<=165,'dock height at '+width+': '+g.dock);
   assert.ok(g.inside,'traffic pane fits the favorites area at '+width);
   assert.ok(g.barInside,'traffic bar fits the dock at '+width);
   assert.ok(g.listRoom>=2*g.row,'two rows of aircraft show under the bar at '+width+': '+JSON.stringify(g));
  }
  assert.deepEqual(errors,[]);

  // No profile for this aircraft: Traffic is the only tab and opens by itself.
  const bare=await open({noProfile:true});
  await freeze(bare.page,100);
  await bare.page.waitForFunction(()=>document.getElementById('mc-dock-traffic'));
  assert.equal(await bare.page.locator('#mc-dock-categories button').count(),1,'only the Traffic tab');
  assert.equal(await bare.page.locator('#mc-dock-traffic-tab').getAttribute('aria-pressed'),'true');
  await bare.page.waitForFunction(n=>document.querySelectorAll('.mc-dock-traffic-aircraft').length===n,sorted.length);
  // Until the module allows spotting without a profile, the pane says why the views are unavailable.
  if(await bare.page.locator('.mc-dock-traffic-aircraft').first().isDisabled())
   assert.match(await bare.page.locator('#mc-dock-traffic .mc-note').innerText(),/profile/);
  assert.deepEqual(bare.errors,[]);
  console.log('Mini Traffic tab: category entry and count, nearest-first pane, readable rows, fixed view/filter bar, status filters, search, view gating, spot / re-aim / stop, leaving the tab, empty state and no-profile case passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
