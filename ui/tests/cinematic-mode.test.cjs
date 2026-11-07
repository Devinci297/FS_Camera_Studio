const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Cinematic mode: the global toggle next to Auto switch in both panels, the Settings section (dwell range, cockpit
// share, cuts, Commercial drift), the per-camera "Include in Cinematic mode" switch and the binding row with its
// default key, against the demo module's playlist.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url=surface=>'file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface='+surface;
  const open=async(surface,viewport)=>{
   const page=await browser.newPage({viewport:viewport||{width:1400,height:1000}}),errors=[];
   page.on('pageerror',e=>errors.push(e.message));
   await page.goto(url(surface));
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   return {page,errors};
  };
  const {page,errors}=await open('main');
  const camera=name=>page.evaluate(n=>{const c=FSCApp.state.working.profile.cameras.find(c=>c.name===n);return c&&c.id;},name);
  const flag=id=>page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).cinematic,id);
  const choose=id=>page.evaluate(id=>{FSCApp.state.selection.cameraId=id;FSCDemo.setOwnership('Observing');},id);

  // Binding row: listed with its label and the documented default.
  await page.locator('#mc-nav-bindings').click();
  await page.locator('#mc-binding-search').fill('cinematic');
  const row=page.locator('[data-binding=toggleCinematic]');
  assert.equal(await row.count(),1);
  assert.match(await row.innerText(),/Toggle Cinematic mode/);
  assert.equal(await row.locator('[data-slot=primary] .mc-binding-input').innerText(),'Ctrl + Alt + K');
  await page.locator('#mc-binding-search').fill('');

  // Per-camera switch: defaults, then a change marks the camera.
  await page.locator('#mc-nav-cameras').click();
  const pilot=await camera('Pilot view'),pfd=await camera('PFD'),tail=await camera('Tail');
  assert.ok(pilot&&pfd&&tail);
  assert.equal(await flag(pilot),true,'the default pilot view takes part');
  assert.equal(await flag(pfd),false,'other cockpit views do not by default');
  assert.equal(await flag(tail),true,'exterior views do');
  await choose(pfd);
  await page.locator('#mc-tab-behavior').click();
  const include=page.getByRole('checkbox',{name:'Include in Cinematic mode',exact:true});
  assert.equal(await include.count(),1);
  assert.equal(await include.isChecked(),false);
  assert.match(await page.locator('#mc-cinematic-note').textContent(),/Cockpit views appear occasionally/);
  await include.click();
  assert.equal(await flag(pfd),true);
  await page.waitForTimeout(400);
  await include.click();
  assert.equal(await flag(pfd),false);
  await page.waitForTimeout(400);
  await choose(tail);
  assert.equal(await include.isChecked(),true);
  assert.equal(await include.isEnabled(),true);

  // Settings section: defaults, min <= max kept, cockpit share in percent, both switches.
  await page.locator('#mc-nav-settings').click();
  await page.locator('#mc-settings-link-cinematic').click();
  assert.ok(await page.locator('#mc-settings-section-cinematic').isVisible());
  assert.equal(await page.evaluate(()=>document.getElementById('mc-settings-link-airport').nextElementSibling.id),'mc-settings-link-cinematic');
  const value=id=>page.locator('#'+id).inputValue();
  assert.equal(await value('mc-setting-cinematic-min'),'8');
  assert.equal(await value('mc-setting-cinematic-max'),'20');
  assert.equal(await value('mc-setting-cinematic-cockpit'),'15');
  assert.equal(await page.locator('#mc-setting-cinematic-cuts').isChecked(),true);
  assert.equal(await page.locator('#mc-setting-cinematic-drift').isChecked(),true);
  const host=()=>page.evaluate(()=>FSCApp.state.working.settings.host);
  await page.locator('#mc-setting-cinematic-min').fill('30');await page.locator('#mc-setting-cinematic-min').press('Tab');
  let h=await host();
  assert.equal(h.cinematicMinSeconds,30);assert.equal(h.cinematicMaxSeconds,30,'the longest time follows the shortest');
  await page.locator('#mc-setting-cinematic-max').fill('6');await page.locator('#mc-setting-cinematic-max').press('Tab');
  h=await host();
  assert.equal(h.cinematicMaxSeconds,6);assert.equal(h.cinematicMinSeconds,6);
  await page.locator('#mc-setting-cinematic-min').fill('1');await page.locator('#mc-setting-cinematic-min').press('Tab');
  assert.equal((await host()).cinematicMinSeconds,3,'clamped to 3 seconds');
  await page.locator('#mc-setting-cinematic-cockpit').fill('40');await page.locator('#mc-setting-cinematic-cockpit').press('Tab');
  assert.equal((await host()).cinematicCockpitWeight,0.4);
  await page.locator('#mc-setting-cinematic-cuts').click();await page.locator('#mc-setting-cinematic-drift').click();
  h=await host();
  assert.equal(h.cinematicCuts,false);assert.equal(h.cinematicDrift,false);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.dirty.settings||document.getElementById('mc-save-state').dataset.dirty==='true'),true,'settings changes are unsaved edits');

  // Main panel toggle sits right after Auto switch and survives page changes.
  const toggle=page.locator('#mc-editor-mouse-look-cinematic');
  assert.ok(await toggle.isVisible());
  assert.equal(await page.evaluate(()=>document.getElementById('mc-editor-mouse-look-auto').nextElementSibling.id),'mc-editor-mouse-look-cinematic');
  assert.equal(await toggle.getAttribute('aria-pressed'),'false');
  assert.equal((await toggle.innerText()).trim(),'Cinematic');
  await toggle.click();
  await page.waitForFunction(()=>FSCApp.state.cinematic&&FSCApp.state.cinematic.enabled);
  await page.waitForFunction(()=>FSCApp.state.ownership.state==='Active');
  assert.equal(await toggle.getAttribute('aria-pressed'),'true');
  await page.waitForFunction(()=>/Cinematic: Parked/.test(document.getElementById('mc-editor-mouse-look-cinematic').textContent));
  const shown=await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(v=>v.id===s.active.cameraId);return c&&c.cinematic;});
  assert.equal(shown,true,'the playlist shows a view that takes part');
  // The demo playlist moves on after the dwell (6 s here), never to the same view.
  const first=await page.evaluate(()=>FSCApp.state.active.cameraId);
  await page.waitForFunction(id=>FSCApp.state.active.cameraId&&FSCApp.state.active.cameraId!==id,first,{timeout:9000});
  // A manual recall stops it.
  await page.evaluate(id=>FSCApp.bridge.request('ActivateCamera',{cameraId:id}),pilot);
  await page.waitForFunction(()=>FSCApp.state.cinematic&&!FSCApp.state.cinematic.enabled);
  assert.equal(await toggle.getAttribute('aria-pressed'),'false');
  // The binding toggles it (Bindings page Test sends the press).
  await page.evaluate(()=>FSCApp.bridge.request('TestBinding',{binding:{action:'toggleCinematic',viewId:0}}));
  await page.waitForFunction(()=>FSCApp.state.cinematic&&FSCApp.state.cinematic.enabled);
  await page.evaluate(()=>FSCApp.bridge.request('TestBinding',{binding:{action:'toggleCinematic',viewId:0}}));
  await page.waitForFunction(()=>FSCApp.state.cinematic&&!FSCApp.state.cinematic.enabled);
  // Release stops it.
  await toggle.click();
  await page.waitForFunction(()=>FSCApp.state.cinematic&&FSCApp.state.cinematic.enabled);
  await page.evaluate(()=>FSCApp.bridge.request('ReleaseCamera',{}));
  await page.waitForFunction(()=>FSCApp.state.cinematic&&!FSCApp.state.cinematic.enabled);
  assert.match(await page.evaluate(()=>FSCApp.state.cinematic.reason),/released/);
  assert.deepEqual(errors,[]);
  await page.close();

  // Mini panel: the toggle sits next to Auto switch and sends SetCinematic.
  const mini=await open('mini',{width:1200,height:1000});
  await mini.page.evaluate(()=>{window.calls=[];const original=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=(a,p)=>{calls.push({a,p});return original(a,p);};document.getElementById('mc-mini-pin').click();});
  const miniToggle=mini.page.locator('#mc-mouse-look-cinematic');
  await miniToggle.waitFor();
  assert.ok(await miniToggle.isVisible());
  assert.equal(await mini.page.evaluate(()=>document.getElementById('mc-mouse-look-auto').nextElementSibling.id),'mc-mouse-look-cinematic');
  await miniToggle.click();
  await mini.page.waitForFunction(()=>calls.some(c=>c.a==='SetCinematic'&&c.p.enabled===true));
  await mini.page.waitForFunction(()=>document.getElementById('mc-mouse-look-cinematic').getAttribute('aria-pressed')==='true');
  await miniToggle.click();
  await mini.page.waitForFunction(()=>calls.some(c=>c.a==='SetCinematic'&&c.p.enabled===false));
  await mini.page.waitForFunction(()=>document.getElementById('mc-mouse-look-cinematic').getAttribute('aria-pressed')==='false');
  const dock=await mini.page.locator('#mc-mini-panel').boundingBox();
  assert.ok(dock.height<=122,'the dock stays compact with the extra toggle ('+dock.height+')');
  assert.deepEqual(mini.errors,[]);
  console.log('Cinematic mode toggle, settings, camera switch and binding row passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
