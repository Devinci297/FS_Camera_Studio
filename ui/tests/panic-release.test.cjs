const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
const RELEASED='Camera released - simulator has control';
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
  const toast=page=>page.evaluate(()=>({visible:!document.getElementById('mc-toast').hidden,text:document.getElementById('mc-toast-text').textContent}));
  const hideToast=page=>page.evaluate(()=>{document.getElementById('mc-toast').hidden=true;});
  const enable=async page=>{
   await page.evaluate(()=>FSCApp.bridge.request('EnableCamera',{}));
   await page.waitForFunction(()=>FSCApp.state.ownership.state==='Active');
   await hideToast(page);
  };
  // The module dispatches a pressed binding exactly like TestBinding: no panel interaction is involved.
  const panic=page=>page.evaluate(()=>FSCApp.bridge.request('TestBinding',{binding:{action:'panicRelease',viewId:0}}));
  const released=page=>page.waitForFunction(text=>!document.getElementById('mc-toast').hidden&&document.getElementById('mc-toast-text').textContent===text,RELEASED,{timeout:2000});

  const {page,errors}=await open('main');
  // The binding is listed with its label and the documented default.
  await page.locator('#mc-nav-bindings').click();
  await page.locator('#mc-binding-search').fill('panic');
  const row=page.locator('[data-binding=panicRelease]');
  assert.equal(await row.count(),1);
  assert.match(await row.innerText(),/Panic: release camera and restore simulator/);
  assert.equal(await row.locator('[data-slot=primary] .mc-binding-input').innerText(),'Ctrl + Alt + X');
  assert.equal(await page.locator('.mc-binding-row').count(),1);

  // Panic from Active: released, toast for about three seconds, then hidden.
  await enable(page);
  const ack=await panic(page);
  assert.equal(ack.status,'Completed');
  await released(page);
  await page.waitForFunction(()=>FSCApp.state.ownership.state==='Observing');
  const shownAt=Date.now();
  await page.waitForTimeout(2000);
  assert.equal((await toast(page)).visible,true,'Toast stays up for about three seconds');
  await page.waitForFunction(()=>document.getElementById('mc-toast').hidden,null,{timeout:2500});
  assert.ok(Date.now()-shownAt<4000);
  assert.equal(await page.evaluate(()=>FSCApp.state.active.cameraId),null);

  // Panic while already Observing: still succeeds and confirms, without taking control.
  assert.equal((await panic(page)).status,'Completed');
  await released(page);
  assert.equal(await page.evaluate(()=>FSCApp.state.ownership.state),'Observing');
  await page.waitForTimeout(600);
  assert.equal(await page.evaluate(()=>FSCApp.state.ownership.state),'Observing');

  // The Release button and an automatic hand-back show the same confirmation.
  await enable(page);
  await page.locator('#mc-release').click();
  await released(page);
  await enable(page);
  await page.evaluate(()=>FSCApp.bridge.demo.setOwnership('Observing','The simulator took the camera back. Select a view to continue.'));
  await released(page);
  // Failed acquisition is not a release.
  await hideToast(page);
  await page.evaluate(()=>{FSCApp.bridge.demo.setOwnership('Acquiring');FSCApp.bridge.demo.setOwnership('Observing','The simulator did not hand over the camera in time. Try again.');});
  assert.equal((await toast(page)).visible,false);
  assert.deepEqual(errors,[]);

  // The mini panel confirms a panic too.
  const mini=await open('mini');
  await enable(mini.page);
  await panic(mini.page);
  await released(mini.page);
  assert.deepEqual(mini.errors,[]);
  console.log('Panic binding listing, release toast timing, idempotent panic, Release button, automatic hand-back and mini toast passed: '+(process.argv[2]||'source'));
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
