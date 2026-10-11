const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright'),assert=require('node:assert/strict'),path=require('node:path');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
for(const surface of ['main','mini']){
 const page=await browser.newPage({viewport:{width:900,height:650}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
 const file=process.argv[2] ? process.argv[2].replace(/main\.html$/,surface+'.html') : 'package/validated-host/html_ui/InGamePanels/FSCameraStudio/'+surface+'.html';
 await page.goto('file:///'+path.resolve(file).replaceAll('\\','/')+'?surface='+surface);await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
 await page.evaluate(()=>{FSCApp.state.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();});
 const primary=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.find(b=>b.action==='selectView'&&b.viewId===1).input.definition);
 if(surface==='mini')await page.locator('#mc-dock-favorites [data-favorite]').first().click({button:'right'});else await page.locator('#mc-camera-menu').click();
 await page.getByRole('button',{name:'Assign key binding',exact:true}).click();await page.locator('.mc-assign-slots [data-slot="secondary"]').click();await page.locator('#mc-assign-device').click();await page.locator('#mc-controller-choices [data-device-id="2"]').click();
 assert.equal(await page.locator('#mc-assign-listen').innerText(),'Listen for button or hat');await page.locator('#mc-assign-listen').click();await page.waitForFunction(()=>FSCApp.state.assign.listening);
 assert.match(await page.locator('#mc-assign-test').innerText(),/move a hat.*return to center/);
 await page.waitForTimeout(100);await page.evaluate(()=>FSCDemo.controllerCaptureStatus('error'));await page.waitForFunction(()=>!FSCApp.state.assign.listening);assert.match(await page.locator('#mc-assign-test').innerText(),/could not register controller listening/);assert.equal(await page.locator('#mc-assign-listen').innerText(),'Listen for button or hat');
 await page.locator('#mc-assign-listen').click();await page.waitForFunction(()=>FSCApp.state.assign.listening);await page.waitForTimeout(100);
 await page.evaluate(()=>FSCDemo.captureControllerHat(1,9000));assert.equal(await page.evaluate(()=>FSCApp.state.assign.staged),null,'Wrong controller hat ignored');
 await page.evaluate(()=>FSCDemo.captureControllerHat(2,4294967295));assert.equal(await page.evaluate(()=>FSCApp.state.assign.staged),null,'Initial centered hat ignored');
 await page.evaluate(()=>FSCDemo.captureControllerHat(2,9000));assert.equal(await page.evaluate(()=>FSCApp.state.assign.staged),null,'Hat must return to center');
 await page.evaluate(()=>FSCDemo.captureControllerHat(2,4294967295));await page.waitForFunction(()=>FSCApp.state.assign.staged&&FSCApp.state.assign.staged.pov==='right');
 assert.match(await page.locator('#mc-assign-test').innerText(),/Detected .*Hat 1 right/);assert.equal(await page.locator('#mc-assign-listen').innerText(),'Listen for another input');
 await page.screenshot({path:'build/review/hat-capture-'+surface+'.png'});await page.locator('#mc-dialog-actions').getByRole('button',{name:'Assign',exact:true}).click();
 const bindings=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.filter(b=>b.action==='selectView'&&b.viewId===1));assert.equal(bindings.length,2);assert.equal(bindings.find(b=>b.slot==='primary').input.definition,primary);assert.equal(bindings.find(b=>b.slot==='secondary').input.definition,'joystick:2:POV:0');assert.equal(bindings.find(b=>b.slot==='secondary').input.pov,'right');
 assert.deepEqual(errors,[]);await page.close();
}
console.log('PASS main and mini: selected controller hat requires direction then center, captures secondary POV right and preserves primary keyboard binding.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
