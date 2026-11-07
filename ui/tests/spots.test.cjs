const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Traffic page Spots: the runway spot picker (airport, end, type, default offsets), Watch from here, live edits with
// AdjustSpot (throttled), Auto, saved spots (save, rename, delete, watch) kept in host.spottingSpots without a Save,
// position spots from the current view (view "spot", North/East/Up nudges), the row grid with aligned columns and
// visible reasons for unavailable views, and saving spots with no camera profile. Runs against the demo module.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main';
  const open=async(options)=>{
   const o=options||{},page=await browser.newPage({viewport:{width:o.width||1400,height:1100}}),errors=[];
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
   await page.evaluate(()=>{window.calls=[];const original=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=(a,p)=>{calls.push({a,p:JSON.parse(JSON.stringify(p||{})),at:Date.now()});return original(a,p);};});
   await page.evaluate(()=>FSCDemo.traffic({time:100}));
   return {page,errors};
  };
  const callsOf=(page,action)=>page.evaluate(a=>calls.filter(c=>c.a===a),action);
  // Waits until the panel sent `action` with a payload that passes `check` (checked here: the page's CSP has no eval).
  const sent=async(page,action,check)=>{
   for(const end=Date.now()+15000;Date.now()<end;await page.waitForTimeout(50))
    if((await callsOf(page,action)).some(c=>!check||check(c.p)))return;
   throw new Error('not sent: '+action+' '+(check||''));
  };
  const spot=page=>page.evaluate(()=>FSCApp.state.spotting||{state:'off'});
  const idle=page=>page.waitForFunction(()=>!FSCApp.state.inflight&&!FSCApp.state.queue.length);

  const {page,errors}=await open();
  await page.locator('#mc-nav-traffic').click();
  const rows=page.locator('#mc-traffic-list .mc-traffic-row'),pane=page.locator('#mc-spots');
  await page.waitForFunction(()=>document.querySelectorAll('#mc-traffic-list .mc-traffic-row').length>=6);
  assert.equal(await pane.getAttribute('open'),'','the Spots panel starts open');

  // Rows: a grid with aligned columns and labels; plain ASCII text.
  assert.equal(await rows.first().evaluate(r=>getComputedStyle(r).display),'grid');
  assert.deepEqual(await page.locator('#mc-traffic-head').evaluate(h=>Array.from(h.querySelectorAll('span')).filter(s=>!s.children.length).map(s=>s.textContent)),['Aircraft','Status','Distance','Altitude','Speed','Views']);
  const columns=await rows.evaluateAll(list=>list.map(r=>['.mc-traffic-status','.mc-traffic-distance','.mc-traffic-altitude','.mc-traffic-speed'].map(sel=>Math.round(r.querySelector(sel).getBoundingClientRect()[sel==='.mc-traffic-status'?'left':'right']))));
  columns.forEach(c=>assert.deepEqual(c,columns[0],'every row uses the same columns'));
  const head=await page.locator('#mc-traffic-head').evaluate(h=>['.mc-traffic-status'].map(sel=>Math.round(h.querySelector(sel).getBoundingClientRect().left)));
  assert.equal(head[0],columns[0][0],'the Status label sits over the status chips');
  assert.ok(await page.evaluate(()=>/^[\x20-\x7e\n\u00b0]*$/.test(document.getElementById('mc-page-traffic').innerText)),'ASCII text only (and the degree sign)');

  // Unavailable views say why, visibly: away from the airport Tower and Runway have no data and Track has no world view.
  const reasons=page.locator('#mc-traffic-reasons');
  assert.ok(await reasons.isVisible());
  const reasonText=await reasons.innerText();
  assert.match(reasonText,/Tower: No tower position is known/);
  assert.match(reasonText,/Runway: No runway data is known/);
  assert.match(reasonText,/Track: Track aims the current airport or free camera/);
  // Without runway data the picker has nothing to choose and Watch from here says why.
  assert.equal(await page.locator('#mc-spot-watch').isDisabled(),true);
  assert.match(await page.locator('#mc-spot-note').innerText(),/No runway data is known/);
  assert.equal(await page.locator('#mc-spot-airport').isDisabled(),true);

  // Near the airport: the airport and its ends (from SpottingChanged.runways), Auto first.
  await page.evaluate(()=>FSCDemo.arriveAtAirport(true));
  await page.waitForFunction(()=>FSCApp.state.spotting&&(FSCApp.state.spotting.runways||[]).length===2);
  await page.waitForFunction(()=>document.getElementById('mc-spot-airport').value==='KDEM');
  assert.deepEqual(await page.locator('#mc-spot-end option').allTextContents(),['Auto','09 (090°)','27 (270°)']);
  assert.match(await reasons.innerText(),/^Track:/,'only Track is unavailable now');
  assert.equal(await page.locator('#mc-spot-end').inputValue(),'');
  assert.equal(await page.locator('#mc-spot-kind [data-kind=fence]').isDisabled(),true,'Auto always uses the fence line');
  assert.match(await page.locator('#mc-spot-help').innerText(),/^Auto picks the end/);
  // The Watch target: the nearest arrival, or the aircraft picked by its name in the list.
  assert.match(await page.locator('#mc-spot-note').innerText(),/^Watch from here shows N456AB \(the nearest arrival\)/);
  await rows.filter({hasText:'easyJet 2914'}).locator('.mc-traffic-name').click();
  assert.equal(await rows.filter({hasText:'easyJet 2914'}).getAttribute('data-selected'),'true');
  assert.match(await page.locator('#mc-spot-note').innerText(),/^Watch from here shows easyJet 2914 \(picked in the list\)/);

  // Types and their default offsets.
  const offsets=()=>page.evaluate(()=>['a','b','c'].map(k=>Number(document.getElementById('mc-spot-offset-'+k).value)));
  const labels=()=>page.locator('#mc-spot-offsets .mc-spot-offset-label').allTextContents();
  await page.locator('#mc-spot-end').selectOption('09');
  assert.equal(await page.locator('#mc-spot-kind [data-kind=fence]').getAttribute('aria-pressed'),'true');
  assert.deepEqual(await labels(),['Behind','Side','Height']);
  assert.deepEqual(await offsets(),[150,80,3]);
  assert.match(await page.locator('#mc-spot-help').innerText(),/^Fence line default: 150 m behind the threshold, 80 m right, 3\.0 m up\..*negative: past it, over the runway.*\+ right, - left/);
  await page.locator('#mc-spot-kind [data-kind=approach]').click();
  assert.deepEqual(await offsets(),[1852,0,7.1]);
  await page.locator('#mc-spot-kind [data-kind=threshold]').click();
  assert.deepEqual(await offsets(),[0,34.5,3],'beside a 45 m wide runway');
  assert.equal(await page.locator('#mc-spot-kind [data-kind=threshold]').getAttribute('aria-pressed'),'true');

  // Watch from here: SpotTarget with the pinned spot for the chosen aircraft; the status line names the spot.
  await page.locator('#mc-spot-watch').click();
  await sent(page,'SpotTarget',p=>p.objectId===104&&p.view==="runway"&&p.spot&&p.spot.runwayEnd==="09"&&p.spot.kind==="threshold"&&p.spot.sideMeters===34.5);
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='tracking'&&FSCApp.state.spotting.spot&&FSCApp.state.spotting.spot.pinned);
  assert.equal(await page.locator('#mc-spot-state-text').innerText(),'Spotting easyJet 2914 - A319 - Runway - KDEM 09 threshold side');
  assert.equal(await rows.filter({hasText:'easyJet 2914'}).getAttribute('data-tracking'),'true');
  assert.equal(await pane.getAttribute('data-live'),'true');
  assert.match(await page.locator('#mc-spot-note').innerText(),/changes apply as you edit/);

  // Live edits: typing sends AdjustSpot at most every 100 ms, the last value always arrives, the target and view stay.
  const before=(await callsOf(page,'AdjustSpot')).length;
  await page.evaluate(async()=>{const input=document.getElementById('mc-spot-offset-a');input.focus();for(let i=1;i<=25;i++){input.value=String(i*10);input.dispatchEvent(new Event('input',{bubbles:true}));await new Promise(r=>setTimeout(r,12));}});
  await page.waitForFunction(()=>FSCApp.state.spotting.spot.behindMeters===250);
  await page.waitForTimeout(300);
  const adjusts=(await callsOf(page,'AdjustSpot')).slice(before);
  assert.ok(adjusts.length>=2&&adjusts.length<=6,'throttled: '+adjusts.length+' AdjustSpot for 25 edits in 300 ms');
  for(let i=1;i<adjusts.length;i++)assert.ok(adjusts[i].at-adjusts[i-1].at>=90,'at most every 100 ms');
  assert.equal(adjusts[adjusts.length-1].p.spot.behindMeters,250);
  assert.equal(adjusts[adjusts.length-1].p.spot.kind,'custom','an edited offset makes the spot custom');
  assert.equal(await page.locator('#mc-spot-kind [data-kind=custom]').getAttribute('aria-pressed'),'true');
  let s=await spot(page);
  assert.equal(s.targetId,104);assert.equal(s.view,'runway');assert.equal(s.spot.kind,'custom');
  assert.equal((await callsOf(page,'SpotTarget')).filter(c=>c.p.objectId===104).length,1,'no restart while editing');
  // Out-of-range values are clamped on commit; -/+ step the value; another end applies live too.
  await page.locator('#mc-spot-offset-c').fill('5000');
  await page.locator('#mc-spot-offset-c').press('Tab');
  assert.equal(await page.locator('#mc-spot-offset-c').inputValue(),'1000');
  await page.waitForFunction(()=>FSCApp.state.spotting.spot.heightMeters===1000);
  await page.locator('#mc-spot-offsets .mc-spot-less').nth(2).click();
  await page.waitForFunction(()=>FSCApp.state.spotting.spot.heightMeters===999);
  await page.locator('#mc-spot-end').selectOption('27');
  await page.waitForFunction(()=>FSCApp.state.spotting.spot.runwayEnd==='27');
  assert.equal((await spot(page)).targetId,104);
  assert.equal(await page.locator('#mc-spot-state-text').innerText(),'Spotting easyJet 2914 - A319 - Runway - KDEM 27 custom spot');

  // Save it: kept in host.spottingSpots at once (no unsaved changes), listed with its offsets.
  const savedBefore=(await page.evaluate(()=>FSCDemo.savedSpots)).length;
  await page.locator('#mc-spot-save').click();
  await idle(page);
  await page.waitForFunction(n=>FSCDemo.savedSpots.length===n+1,savedBefore);
  const savedRows=page.locator('#mc-spot-saved .mc-spot-saved-row');
  assert.equal(await savedRows.count(),savedBefore+1);
  const mine=savedRows.filter({hasText:'KDEM 27 custom spot'});
  assert.match(await mine.locator('.mc-spot-saved-detail').innerText(),/^KDEM 27 - custom spot - 250 m behind, 34\.5 m right, 999 m up$/);
  assert.equal(await mine.getAttribute('data-editing'),'true');
  assert.equal(await page.locator('#mc-save-state').getAttribute('data-dirty'),'false','saved spots are never unsaved edits');
  assert.equal(await page.evaluate(()=>FSCApp.state.working.dirty.settings),false);
  assert.ok(await page.locator('#mc-spot-update').isVisible());
  assert.match(await page.locator('#mc-spot-saved-head').innerText(),new RegExp('Saved spots \\('+(savedBefore+1)+'\\)'));
  // Rename: an inline field, Enter saves; the live view takes the name.
  await mine.locator('[data-action=rename]').click();
  const rename=page.locator('#mc-spot-saved .mc-spot-rename');
  await rename.waitFor();
  assert.equal(await page.evaluate(()=>document.activeElement.classList.contains('mc-spot-rename')),true);
  await rename.fill('Far end fence');
  await rename.press('Enter');
  await idle(page);
  await page.waitForFunction(()=>FSCDemo.savedSpots.some(s=>s.name==='Far end fence'));
  await page.waitForFunction(()=>/Far end fence$/.test(document.getElementById('mc-spot-state-text').textContent));
  assert.equal(await savedRows.filter({hasText:'Far end fence'}).getAttribute('data-active'),'true');
  // Escape cancels a rename.
  await savedRows.filter({hasText:'Far end fence'}).locator('[data-action=rename]').click();
  await rename.fill('Nope');
  await rename.press('Escape');
  assert.equal(await savedRows.filter({hasText:'Nope'}).count(),0);

  // Auto: back to the automatic end selection for the same aircraft.
  await page.locator('#mc-spot-end').selectOption('');
  await sent(page,'SpotTarget',p=>p.objectId===104&&p.view==="runway"&&!p.spot);
  // (the loaded airport's own runway view here; a computed one names the end: "Runway - Auto KDEM 09")
  await page.waitForFunction(()=>FSCApp.state.spotting.view==='runway'&&!(FSCApp.state.spotting.spot&&FSCApp.state.spotting.spot.pinned));
  assert.match(await page.locator('#mc-spot-state-text').innerText(),/^Spotting easyJet 2914 - A319 - Runway( - Auto KDEM (09|27))?$/);
  assert.equal(await pane.getAttribute('data-live'),'false');
  const quiet=(await callsOf(page,'AdjustSpot')).length;
  await page.waitForTimeout(300);
  assert.equal((await callsOf(page,'AdjustSpot')).length,quiet,'nothing to adjust on the automatic view');

  // A saved spot's Watch watches the current target from it; Edit loads it without watching.
  await savedRows.filter({hasText:'KDEM 27 fence line'}).locator('[data-action=edit]').click();
  assert.equal(await page.locator('#mc-spot-end').inputValue(),'27');
  assert.equal(await page.locator('#mc-spot-kind [data-kind=fence]').getAttribute('aria-pressed'),'true');
  await savedRows.filter({hasText:'Far end fence'}).locator('[data-action=watch]').click();
  await sent(page,'SpotTarget',p=>p.objectId===104&&p.spot&&p.spot.name==="Far end fence"&&p.spot.behindMeters===250);
  await page.waitForFunction(()=>FSCApp.state.spotting.spot&&FSCApp.state.spotting.spot.name==='Far end fence');
  // Delete removes it from the settings.
  await savedRows.filter({hasText:'Far end fence'}).locator('[data-action=delete]').click();
  await idle(page);
  await page.waitForFunction(n=>FSCDemo.savedSpots.length===n,savedBefore);
  assert.equal(await savedRows.filter({hasText:'Far end fence'}).count(),0);

  // Position spot: save the current (spotting) view, watch from it in the "spot" view, nudge it North / East / Up.
  await page.locator('#mc-spot-here').click();
  await sent(page,'DroneHere');
  await page.waitForFunction(n=>FSCDemo.savedSpots.length===n+1&&FSCDemo.savedSpots.some(s=>s.kind==='position'),savedBefore);
  const place=(await page.evaluate(()=>FSCDemo.savedSpots)).filter(s=>s.kind==='position')[0];
  assert.equal(place.name,'KDEM view 1');assert.equal(place.icao,'KDEM');
  assert.ok(isFinite(place.latitude)&&isFinite(place.longitude)&&isFinite(place.altitudeMeters));
  assert.equal(await pane.getAttribute('data-mode'),'position');
  assert.ok(await page.locator('#mc-spot-picker').isHidden());
  assert.deepEqual(await labels(),['North','East','Up']);
  assert.deepEqual(await offsets(),[0,0,0]);
  assert.match(await page.locator('#mc-spot-place').innerText(),/^KDEM view 1: /);
  assert.match(await page.locator('#mc-spot-help').innerText(),/North, East and Up move this spot in meters/);
  await page.locator('#mc-spot-watch').click();
  await sent(page,'SpotTarget',p=>p.view==="spot"&&p.spot&&p.spot.kind==="position"&&p.spot.name==="KDEM view 1");
  await page.waitForFunction(()=>FSCApp.state.spotting.view==='spot');
  assert.equal(await page.locator('#mc-spot-state-text').innerText(),'Spotting easyJet 2914 - A319 - Saved view - KDEM view 1');
  assert.equal(await pane.getAttribute('data-live'),'true');
  const adjusted=await page.evaluate(()=>FSCDemo.spotAdjusts);
  await page.locator('#mc-spot-offsets .mc-spot-more').nth(0).click();
  await page.waitForFunction(n=>FSCDemo.spotAdjusts>n,adjusted);
  s=await spot(page);
  assert.ok(Math.abs((s.spot.latitude-place.latitude)*111320-10)<0.05,'10 m north');
  assert.ok(Math.abs(s.spot.longitude-place.longitude)<1e-9);
  await page.locator('#mc-spot-offset-c').fill('25');
  await page.locator('#mc-spot-offset-c').press('Tab');
  await page.waitForFunction(alt=>Math.abs(FSCApp.state.spotting.spot.altitudeMeters-(alt+25))<0.05,place.altitudeMeters);
  // Update keeps the nudge in the saved spot; the editor starts from there again.
  await page.locator('#mc-spot-update').click();
  await idle(page);
  await page.waitForFunction(alt=>FSCDemo.savedSpots.some(s=>s.kind==='position'&&Math.abs(s.altitudeMeters-(alt+25))<0.05),place.altitudeMeters);
  assert.deepEqual(await offsets(),[0,0,0]);
  // Next target keeps watching from the position spot; Track keeps it too.
  await page.evaluate(()=>FSCApp.bridge.request('TestBinding',{binding:{action:'spotNext',viewId:0}}));
  await page.waitForFunction(()=>FSCApp.state.spotting.targetId!==104&&FSCApp.state.spotting.view==='spot');
  assert.equal((await spot(page)).spot.name,'KDEM view 1');
  // Leaving position mode returns to runway spots.
  await page.locator('#mc-spot-leave-position').click();
  assert.equal(await pane.getAttribute('data-mode'),'runway');
  assert.equal(await page.evaluate(()=>FSCApp.state.spotting.view),'spot','leaving the editor mode does not change the view');
  await page.locator('#mc-spot-stop').click();
  await page.waitForFunction(()=>FSCApp.state.spotting.state==='off');
  assert.equal(await pane.getAttribute('data-live'),'false');

  // Narrow panel: rows stack into lines without overflowing, the Spots fields still fit.
  for(const width of [1000,640]){
   await page.setViewportSize({width,height:1100});await page.waitForTimeout(300);
   const g=await rows.evaluateAll(list=>list.map(r=>({over:r.scrollWidth>r.clientWidth+1,name:r.querySelector('.mc-traffic-name').getBoundingClientRect(),views:r.querySelector('.mc-traffic-views').getBoundingClientRect(),figures:r.querySelector('.mc-traffic-figures').getBoundingClientRect()})));
   g.forEach(r=>{assert.equal(r.over,false,'no overflow at '+width);assert.ok(r.views.top>=r.name.bottom-1,'views on their own line at '+width);});
   if(width===640)g.forEach(r=>assert.ok(r.figures.top>=r.name.bottom-1,'figures under the name at 640'));
   assert.equal(await page.locator('#mc-spots').evaluate(p=>p.scrollWidth<=p.clientWidth+1),true);
  }
  assert.deepEqual(errors,[]);
  await page.close();

  // No camera profile: spots are saved the same way, with no profile and no Save.
  const bare=await open({noProfile:true});
  await bare.page.locator('#mc-nav-traffic').click();
  await bare.page.evaluate(()=>FSCDemo.arriveAtAirport(true));
  await bare.page.waitForFunction(()=>FSCApp.state.working&&!FSCApp.state.working.profile&&(FSCApp.state.spotting.runways||[]).length===2);
  await bare.page.waitForFunction(()=>document.getElementById('mc-spot-airport').value==='KDEM');
  await bare.page.locator('#mc-spot-end').selectOption('09');
  await bare.page.locator('#mc-spot-kind [data-kind=approach]').click();
  await bare.page.locator('#mc-spot-save').click();
  await sent(bare.page,'UpdateDraft',p=>p.ops.some(o=>o.op==="setSettings"&&(o.settings.host.spottingSpots||[]).some(s=>s.kind==="approach")));
  await bare.page.waitForFunction(()=>FSCDemo.savedSpots.some(s=>s.kind==='approach'&&s.name==='KDEM 09 approach 1 NM'));
  await idle(bare.page);
  assert.equal(await bare.page.locator('#mc-save-state').getAttribute('data-dirty'),'false');
  // Watching from it works without a profile.
  await bare.page.locator('#mc-spot-watch').click();
  await bare.page.waitForFunction(()=>FSCApp.state.spotting.state==='tracking'&&FSCApp.state.spotting.spot&&FSCApp.state.spotting.spot.kind==='approach');
  assert.deepEqual(bare.errors,[]);
  console.log('Spots: picker, defaults, Watch from here, throttled live edits, Auto, saved spots (save, rename, delete, watch, edit), position spots and nudges, grid rows, visible reasons and no-profile saving passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
