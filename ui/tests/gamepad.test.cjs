const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Settings > Gamepad: the section after Movement, the two switches and the dead zone, choosing a controller, Listen
// for each drone motion (the demo module answers with a gamepad's axes), Clear and Clear all.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main';
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(url);
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const host=()=>page.evaluate(()=>FSCApp.state.working.settings.host);
  await page.locator('#mc-nav-settings').click();
  assert.equal(await page.evaluate(()=>document.getElementById('mc-settings-link-movement').nextElementSibling.id),'mc-settings-link-gamepad');
  await page.locator('#mc-settings-link-gamepad').click();
  const pane=page.locator('#mc-settings-section-gamepad');
  assert.equal(await pane.isVisible(),true);
  assert.equal(await page.locator('#mc-setting-gamepad-enabled').isChecked(),true);
  assert.equal(await page.locator('#mc-setting-gamepad-block').isChecked(),true);
  assert.equal(await page.locator('#mc-setting-gamepad-deadzone').inputValue(),'15');
  // Controllers from the module; the first is chosen.
  await page.waitForSelector('#mc-gamepad-devices [data-device-id]');
  assert.equal(await page.locator('#mc-gamepad-devices [aria-pressed=true]').count(),1);
  await page.locator('#mc-gamepad-devices [data-device-id="2"]').click();
  assert.equal(await page.locator('#mc-gamepad-devices [data-device-id="2"]').getAttribute('aria-pressed'),'true');
  const rows=page.locator('.mc-gamepad-row');
  assert.equal(await rows.count(),6);
  assert.match(await page.locator('[data-motion=forward]').innerText(),/Not assigned/);
  // Listen: the row prompts, then the detected axis is assigned to that motion.
  await page.locator('[data-motion=forward] button',{hasText:'Listen'}).click();
  assert.match(await page.locator('[data-motion=forward]').innerText(),/Push the stick you want for this forward/);
  assert.match(await page.locator('[data-motion=forward]').innerText(),/Stop listening/);
  await page.waitForFunction(()=>(FSCApp.state.working.settings.host.gamepadAxes||[]).some(a=>a.function==='forward'));
  let axes=(await host()).gamepadAxes;
  assert.deepEqual(axes.map(a=>[a.function,a.definition,a.rest,a.full]),[['forward','joystick:2:YAxis',0,-32767]]);
  assert.match(await page.locator('[data-motion=forward]').innerText(),/Xbox Controller - Y axis/);
  assert.match(await page.locator('#mc-gamepad-status').innerText(),/Assigned Xbox Controller - Y axis to Move forward and back/);
  for(const fn of ['right','climb','descend','yaw','pitch']){
   await page.locator('[data-motion='+fn+'] button',{hasText:'Listen'}).click();
   await page.waitForFunction(f=>(FSCApp.state.working.settings.host.gamepadAxes||[]).some(a=>a.function===f),fn);
  }
  axes=(await host()).gamepadAxes;
  assert.equal(axes.length,6);
  // Listening again replaces that motion only.
  await page.locator('[data-motion=yaw] button',{hasText:'Listen'}).click();
  await page.waitForFunction(()=>/Assigned .* to Turn right and left/.test(document.getElementById('mc-gamepad-status').textContent));
  assert.equal((await host()).gamepadAxes.filter(a=>a.function==='yaw').length,1);
  // Stop listening before the answer: nothing changes.
  const before=JSON.stringify((await host()).gamepadAxes);
  await page.locator('[data-motion=pitch] button',{hasText:'Listen'}).click();
  await page.locator('[data-motion=pitch] button',{hasText:'Stop listening'}).click();
  await page.waitForTimeout(1200);
  assert.equal(JSON.stringify((await host()).gamepadAxes),before);
  // Clear one, the switches and the dead zone, then Clear all.
  await page.locator('[data-motion=descend] button',{hasText:'Clear'}).click();
  assert.equal((await host()).gamepadAxes.some(a=>a.function==='descend'),false);
  assert.match(await page.locator('[data-motion=descend]').innerText(),/Not assigned/);
  await page.locator('#mc-setting-gamepad-block').uncheck();
  assert.equal((await host()).gamepadBlocksAircraft,false);
  await page.locator('#mc-setting-gamepad-deadzone').fill('25');await page.locator('#mc-setting-gamepad-deadzone').press('Enter');
  await page.locator('#mc-setting-gamepad-deadzone').blur();
  assert.equal((await host()).gamepadDeadzone,0.25);
  await page.locator('#mc-setting-gamepad-enabled').uncheck();
  assert.equal((await host()).gamepadSticksEnabled,false);
  assert.equal(await page.locator('#mc-setting-gamepad-block').isDisabled(),true);
  await page.locator('#mc-gamepad-clear-all').click();
  assert.deepEqual((await host()).gamepadAxes,[]);
  assert.equal(await page.locator('#mc-gamepad-clear-all').isDisabled(),true);
  await page.screenshot({path:process.env.FSCS_SHOT||path.join(require('node:os').tmpdir(),'fscs-gamepad.png')});
  assert.deepEqual(errors,[]);
  console.log('gamepad settings ok');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
