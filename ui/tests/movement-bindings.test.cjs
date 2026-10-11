const {chromium}=require(process.env.FSCS_PLAYWRIGHT || 'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1100,height:1000}}), errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp && FSCApp.state.snapshot);
  await page.locator('[data-precision=coarse]').click();
  await page.waitForFunction(()=>!FSCApp.state.inflight&&!FSCApp.state.queue.length);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.settings.ui.precision),'coarse');
  await page.evaluate(()=>FSCApp.bridge.request('RequestSnapshot',{}));
  await page.waitForFunction(()=>document.querySelector('[data-precision=coarse]').getAttribute('aria-pressed')==='true');
  await page.locator('#mc-nav-bindings').click();
  await page.locator('#mc-binding-search').fill('move');
  const row=action=>page.locator('[data-binding='+action+']');
  for(const [action,key] of [['moveForward','W'],['moveBack','S'],['moveLeft','A'],['moveRight','D'],['moveUp','E'],['moveDown','Q'],['moveFast','Shift']]) {
   assert.equal(await row(action).locator('.mc-binding-input').innerText(),key);
  }
  await row('moveForward').getByRole('button',{name:'Assign an input to Move forward',exact:true}).click();
  await page.locator('#mc-assign-capture').press('i');
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Assign',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.working.bindings.bindings.some(b=>b.action==='moveForward'&&b.input.definition==='I'));
  await row('moveFast').getByRole('button',{name:'Assign an input to Faster movement (hold)',exact:true}).click();
  await page.locator('#mc-assign-capture').press('Shift');
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Assign',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.working.bindings.bindings.some(b=>b.action==='moveFast'&&b.input.definition==='VK_LSHIFT'));
  await row('moveDown').getByRole('button',{name:'Clear Move down',exact:true}).click();
  await page.locator('#mc-binding-search').fill('zoom');
  await row('zoomIn').getByRole('button',{name:'Assign an input to Zoom in',exact:true}).click();
  await page.locator('#mc-assign-capture').dispatchEvent('wheel',{deltaY:100});
  // Existing down assignment is moved atomically to the reversed zoom-in action.
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Replace existing assignment',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.working.bindings.bindings.some(b=>b.action==='zoomIn'&&b.input.definition==='sim:ZOOM_OUT'));
  assert.equal(await page.evaluate(()=>FSCApp.state.working.bindings.bindings.some(b=>b.action==='zoomOut')),false);
  await row('zoomOut').getByRole('button',{name:'Assign an input to Zoom out',exact:true}).click();
  await page.locator('#mc-assign-capture').press('o');
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Assign',exact:true}).click();
  await page.locator('#mc-binding-kind').selectOption('simZoom');
  assert.equal(await page.locator('.mc-binding-row').count(),1);
  await page.locator('#mc-save').click();
  await page.waitForFunction(()=>!FSCApp.state.pendingCommit&&!FSCApp.state.inflight&&!FSCApp.state.queue.length);
  await page.evaluate(()=>FSCApp.bridge.request('RequestSnapshot',{}));
  const state=await page.evaluate(()=>FSCApp.state.working);
  assert.equal(state.settings.ui.precision,'coarse');
  assert.equal(state.bindings.bindings.find(b=>b.action==='moveForward').input.definition,'I');
  assert.equal(state.bindings.bindings.some(b=>b.action==='moveDown'),false);
  assert.equal(state.bindings.bindings.find(b=>b.action==='zoomOut').input.definition,'O');
  assert.equal(state.bindings.movementDefaultsVersion,1);
  assert.deepEqual(errors,[]);
  console.log('Movement/boost defaults, precision persistence, key/wheel reassignment, conflict replacement, clearing and saved snapshot passed: '+(process.argv[2]||'source'));
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
