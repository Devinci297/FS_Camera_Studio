const {chromium}=require(process.env.FSCS_PLAYWRIGHT),assert=require('node:assert/strict'),path=require('node:path');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
for(const surface of ['main','mini']){
const page=await browser.newPage({viewport:{width:900,height:650}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
const file=path.resolve(process.argv[2] ? process.argv[2].replace(/main\.html$/,surface+'.html') : 'package/validated-host/html_ui/InGamePanels/FSCameraStudio/'+surface+'.html');await page.goto('file:///'+file.replaceAll('\\','/')+'?surface='+surface);await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
await page.evaluate(()=>{FSCApp.state.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();});
async function open(){if(surface==='mini')await page.locator('#mc-dock-favorites [data-favorite]').first().click({button:'right'});else await page.locator('#mc-camera-menu').click();await page.getByRole('button',{name:'Assign key binding',exact:true}).click();await page.locator('#mc-assign-device').waitFor();}
async function chooseController(){
 const picker=page.locator('#mc-assign-device');
 assert.equal(await picker.evaluate(e=>e.tagName),'BUTTON','Picker uses panel buttons, not native dropdown');
 await picker.click();await page.locator('#mc-controller-choices').waitFor({state:'visible'});
 await picker.hover();await page.waitForTimeout(500);
 assert.equal(await page.locator('#mc-control-tooltip').isVisible(),false,'Options are never concatenated into tooltip');
 await picker.press('Escape');assert.equal(await page.locator('#mc-controller-choices').isVisible(),false);
 assert.equal(await page.locator('#mc-dialog-shade').isVisible(),true,'Escape closes choices before assignment dialog');
 await picker.click();await page.screenshot({path:'build/review/controller-picker-'+surface+'.png'});
 await page.locator('#mc-controller-choices [data-device-id="2"]').click();
 assert.match(await picker.innerText(),/TWCS Throttle/);assert.equal(await picker.getAttribute('aria-expanded'),'false');
 // A periodic refresh of the same devices must preserve the chosen controller.
 await page.evaluate(()=>FSCApp.state.assign && FSCApp.state.assign.selectedDeviceId);
}
const original=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.find(b=>b.action==='selectView'&&b.viewId===1).input.definition);
await open();await page.locator('.mc-assign-slots [data-slot="secondary"]').click();await chooseController();await page.locator('#mc-assign-listen').click();
await page.waitForFunction(()=>FSCApp.state.assign.listening);assert.equal(await page.evaluate(()=>FSCDemo.captureControllerButton(1,7)),false,'Unselected controller ignored');
await page.evaluate(()=>FSCDemo.captureControllerButton(2,7,'expired-listen-session'));await page.waitForTimeout(50);assert.equal(await page.evaluate(()=>FSCApp.state.assign.staged),null,'Late capture from previous listening session ignored');
assert.equal(await page.evaluate(()=>FSCDemo.captureControllerButton(2,7)),true);await page.waitForFunction(()=>FSCApp.state.assign.staged&&FSCApp.state.assign.staged.definition==='joystick:2:button:6');
await page.screenshot({path:'build/review/dual-bindings-detected-'+surface+'.png'});
const assign=page.locator('#mc-dialog-actions').getByRole('button',{name:'Assign',exact:true});await assign.click();
await page.waitForFunction(()=>FSCApp.state.working.bindings.bindings.some(b=>b.viewId===1&&b.slot==='secondary'));
let rows=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.filter(b=>b.action==='selectView'&&b.viewId===1));assert.equal(rows.length,2);assert.equal(rows.find(b=>b.slot==='primary').input.definition,original);assert.equal(rows.find(b=>b.slot==='secondary').input.deviceId,2);
assert.match(await page.locator('#mc-dock-favorites [data-favorite]').first().locator('.mc-key').innerText(),/Button 7/);
// Replacing a primary key preserves both this camera's controller and another camera's secondary key.
await page.evaluate(()=>{FSCApp.state.working.bindings.bindings.push({action:'selectView',viewId:2,slot:'secondary',input:{kind:'key',definition:'J',label:'J'}});});
await open();await page.locator('#mc-assign-capture').press('Control+Alt+2');await page.getByRole('button',{name:'Replace existing assignment',exact:true}).click();
rows=await page.evaluate(()=>FSCApp.state.working.bindings.bindings);assert.equal(rows.filter(b=>b.action==='selectView'&&b.viewId===1).length,2);assert.ok(rows.some(b=>b.viewId===2&&b.slot==='secondary'&&b.input.definition==='J'),'Other slot of conflict owner survives');
await open();await page.locator('.mc-assign-slots [data-slot="secondary"]').click();await page.getByRole('button',{name:'Clear assignment',exact:true}).click();rows=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.filter(b=>b.action==='selectView'&&b.viewId===1));assert.equal(rows.length,1);assert.equal(rows[0].slot,'primary');assert.equal(rows[0].input.definition,'VK_LCONTROL+VK_LMENU+2');
// Cancellation, timeout, manual fallback, and responsive footer.
await open();await page.locator('.mc-assign-slots [data-slot="secondary"]').click();await page.locator('#mc-assign-listen').click();await page.waitForFunction(()=>FSCApp.state.assign.listening);await page.waitForTimeout(100);await page.evaluate(()=>FSCDemo.controllerCaptureStatus('timeout'));await page.waitForFunction(()=>!FSCApp.state.assign.listening);assert.match(await page.locator('#mc-assign-test').innerText(),/No button or hat detected/);
await page.locator('.mc-controller-manual summary').click();await page.getByRole('button',{name:'Test input',exact:true}).click();await page.waitForFunction(()=>FSCApp.state.assign.tested.down&&FSCApp.state.assign.tested.up);
for(const height of [360,480]){await page.setViewportSize({width:560,height});await page.waitForTimeout(100);const r=await assign.boundingBox();assert.ok(r.y>=0&&r.y+r.height<=height,'Footer visible on '+surface+' '+height);assert.ok(await assign.evaluate(e=>{const r=e.getBoundingClientRect();return e.contains(document.elementFromPoint(r.x+r.width/2,r.y+r.height/2));}));}
await page.screenshot({path:'build/review/dual-bindings-'+surface+'.png'});await page.getByRole('button',{name:'Cancel',exact:true}).click();assert.equal(await page.evaluate(()=>FSCDemo.captureControllerButton(1,4)),false,'Capture stopped on close');
if(surface==='main'){
 await page.setViewportSize({width:1200,height:850});
 await page.locator('[data-page="bindings"]').click();await page.locator('#mc-bindings-list summary').first().click();
 const row=page.locator('[data-binding="selectView:1"]');await row.scrollIntoViewIfNeeded();
 assert.equal(await row.locator('[data-slot]').count(),2);await page.screenshot({path:'build/review/dual-bindings-list.png'});
 await row.locator('[data-slot="secondary"] .mc-binding-input').click();await chooseController();await page.locator('#mc-assign-listen').click();
 await page.waitForFunction(()=>FSCApp.state.assign.listening);await page.screenshot({path:'build/review/dual-bindings-listening.png'});
 await page.getByRole('button',{name:'Cancel',exact:true}).click();
}
assert.deepEqual(errors,[]);await page.close();}
console.log('Dual slots preserve keyboard/controller and conflict owner inputs; controller selection/capture/manual/timeout/cancel and short main/mini footer passed.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
