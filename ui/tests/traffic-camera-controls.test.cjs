const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try {
  for(const surface of ['main','mini']) {
   const page=await browser.newPage({viewport:{width:1100,height:850}}),errors=[];
   page.on('pageerror',e=>errors.push(e.message));
   await page.goto('file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(async()=>{FSCDemo.traffic({time:100});await FSCApp.bridge.request('SpotTarget',{objectId:104,view:'chase'});});
   await page.waitForFunction(()=>FSCApp.state.spotting.state==='tracking'&&FSCApp.state.ownership.state==='Active');
   // The computed traffic camera must expose controls even with no aircraft profile underneath it.
   await page.evaluate(()=>{FSCApp.state.working.profile=null;window.calls=[];const request=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.request=(action,payload)=>{calls.push({action,payload});return request(action,payload);};});
   const trigger=surface==='main'?'#mc-editor-mouse-look':'#mc-mouse-look';
   await page.locator(trigger).evaluate(e=>e.click());
   await page.getByText('Traffic camera controls',{exact:true}).waitFor();
   assert.match(await page.locator('#mc-dialog-body').innerText(),/right-click.*rotate/);
   assert.match(await page.locator('#mc-dialog-body').innerText(),/temporary/);
   assert.equal(await page.locator('#mc-dialog-body .mc-look-choices').count(),0,'does not edit an underlying camera profile');
   await page.getByRole('button',{name:'Forward',exact:true}).click();
   await page.getByRole('button',{name:'Look right',exact:true}).click();
   await page.waitForFunction(()=>calls.some(c=>c.action==='MouseLookDelta'&&c.payload.forwardMeters===.5)&&calls.some(c=>c.action==='MouseLookDelta'&&c.payload.headingDeg===5));
   await page.screenshot({path:'build/review/traffic-camera-controls-'+surface+'.png'});
   await page.getByRole('button',{name:'Recenter',exact:true}).click();
   await page.waitForFunction(()=>calls.some(c=>c.action==='RecenterLook'));
   assert.equal(await page.evaluate(()=>FSCApp.state.spotting.targetId),104);
   await page.locator(trigger).evaluate(e=>e.click());
   const before=await page.evaluate(()=>{FSCApp.state.spotting={state:'off'};return calls.length;});
   await page.getByRole('button',{name:'Forward',exact:true}).click();
   assert.equal(await page.evaluate(()=>calls.length),before,'stale traffic dialog must not move the aircraft camera');
   assert.deepEqual(errors,[]);await page.close();
  }
  console.log('PASS: traffic camera help and recenter on main/mini without a profile; target retained.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
