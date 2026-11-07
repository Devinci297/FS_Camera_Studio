const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const url=surface=>'file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface='+surface;
  const open=async surface=>{
   const page=await browser.newPage({viewport:{width:1100,height:1000}}),errors=[];
   page.on('pageerror',e=>errors.push(e.message));
   await page.goto(url(surface));
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   return {page,errors};
  };
  const enable=async page=>{
   await page.evaluate(()=>FSCApp.bridge.request('EnableCamera',{}));
   await page.waitForFunction(()=>FSCApp.state.ownership.state==='Active');
  };

  const {page,errors}=await open('main');
  // The binding is listed with its label, the documented default and an empty secondary slot.
  await page.locator('#mc-nav-bindings').click();
  await page.locator('#mc-binding-search').fill('binoculars');
  const row=page.locator('[data-binding=binoculars]');
  assert.equal(await row.count(),1);
  assert.match(await row.innerText(),/Binoculars \(hold\)/);
  assert.match(await row.innerText(),/release to return/);
  assert.equal(await row.locator('[data-slot=primary] .mc-binding-input').innerText(),'Ctrl + Alt + Z');
  assert.equal(await row.locator('[data-slot=secondary] .mc-binding-input').innerText(),'Assign input');
  assert.equal(await page.locator('.mc-binding-row').count(),1);
  // Test cannot demonstrate a hold from the panel: it explains how to try it instead.
  await row.locator('[data-slot=primary] button',{hasText:'Test'}).click();
  await page.waitForFunction(()=>/assigned input in the simulator/.test(document.getElementById('mc-binding-result').textContent));
  assert.equal(await page.locator('#mc-binding-result').getAttribute('data-tone'),'warn');
  // No other default uses Ctrl + Alt + Z.
  const zUsers=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.filter(b=>b.input.definition==='VK_LCONTROL+VK_LMENU+Z').map(b=>b.action));
  assert.deepEqual(zUsers,['binoculars']);

  // Settings: Movement holds the binoculars field; values clamp to 2-60 degrees and mark settings unsaved.
  await page.locator('#mc-nav-settings').click();
  await page.locator('#mc-settings-link-movement').click();
  const field=page.locator('#mc-setting-binoculars-fov');
  assert.equal(await field.isVisible(),true);
  assert.equal(await field.inputValue(),'10');
  const setField=async value=>{await field.evaluate((input,v)=>{input.value=v;input.dispatchEvent(new Event('change',{bubbles:true}));},value);};
  const stored=()=>page.evaluate(()=>FSCApp.state.working.settings.host.binocularsFovDeg);
  await setField('1');assert.equal(await stored(),2);assert.equal(await field.inputValue(),'2');
  await setField('100');assert.equal(await stored(),60);assert.equal(await field.inputValue(),'60');
  await setField('25');assert.equal(await stored(),25);
  await page.waitForFunction(()=>FSCApp.state.working&&FSCApp.state.working.dirty&&FSCApp.state.working.dirty.settings===true);
  // The editor's own FOV range is unchanged.
  assert.deepEqual(await page.evaluate(()=>FSCApp.state.caps.fovRangeDeg),[20,120]);
  assert.deepEqual(errors,[]);

  // Mini panel: a badge shows only while binoculars is held on an active camera; release and panic clear it.
  const mini=await open('mini');
  const badge=mini.page.locator('#mc-binoculars-badge');
  assert.equal(await badge.isVisible(),false);
  assert.equal(await mini.page.evaluate(()=>FSCApp.bridge.demo.binoculars(true)),false,'Nothing to zoom while the simulator has control');
  assert.equal(await badge.isVisible(),false);
  await enable(mini.page);
  await mini.page.evaluate(()=>FSCApp.bridge.demo.binoculars(true));
  await badge.waitFor({state:'visible'});
  assert.match(await badge.innerText(),/^Binoculars \d+°$/);
  await mini.page.evaluate(()=>FSCApp.bridge.demo.binoculars(false));
  await badge.waitFor({state:'hidden'});
  await mini.page.evaluate(()=>FSCApp.bridge.demo.binoculars(true));
  await badge.waitFor({state:'visible'});
  await mini.page.evaluate(()=>FSCApp.bridge.request('TestBinding',{binding:{action:'panicRelease',viewId:0}}));
  await badge.waitFor({state:'hidden'});
  assert.deepEqual(mini.errors,[]);
  console.log('Binoculars binding row, default, Settings field and mini indicator passed: '+(process.argv[2]||'source'));
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
