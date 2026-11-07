const {chromium} = require(process.env.FSCS_PLAYWRIGHT || 'playwright');
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs'),{spawn}=require('node:child_process'),readline=require('node:readline');
const root=path.resolve(process.env.XPCS_TEST_BUILD || 'build/traffic-controls/xplane');
const driver=path.join(root,'bin/Release/xpcs_key_service_driver.exe'),helper=path.join(root,'XPCameraStudio/win_x64/XPCSKeyHelper.exe');
const original='I\r\n1005 Version\r\nNumpad-1 <NONE> sim/view/quick_look_1\r\nNumpad-1 <NONE> another/camera\r\nNumpad-1 CTRL sim/view/quick_look_1_mem\r\nNumpad-2 <NONE> xpcs/view_1\r\nW <NONE> sim/view/forward_with_panel\r\n';
const profile='control profiles/Camera test - Keys.prf';
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function waitFor(f){for(let n=0;n<150;n++){if(f())return;await delay(20);}throw new Error('Timed out waiting for native helper result');}
function launch(prefs){
 const child=spawn(driver,[prefs,helper],{windowsHide:true,stdio:['pipe','pipe','pipe']});
 const pending=[];let stderr='';child.stderr.on('data',s=>stderr+=s);
 readline.createInterface({input:child.stdout}).on('line',line=>{const p=pending.shift();if(p){try{p.resolve(JSON.parse(line));}catch(e){p.reject(e);}}});
 const exited=new Promise((resolve,reject)=>{child.on('error',reject);child.on('exit',code=>{pending.splice(0).forEach(p=>p.reject(new Error('Native driver exited: '+stderr)));code===0?resolve():reject(new Error('Native driver '+code+': '+stderr));});});
 return {child,exited,request:payload=>new Promise((resolve,reject)=>{pending.push({resolve,reject});child.stdin.write(JSON.stringify(payload)+'\n');})};
}
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.FSCS_BROWSER,headless:true});
 const file='file:///'+path.resolve('ui/index.html').replaceAll('\\','/');
 const demo=await browser.newPage();await demo.goto(file+'?surface=main');
 await demo.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
 const snapshot=await demo.evaluate(()=>new Promise(resolve=>{FSCApp.bridge.on('event',m=>{if(m.name==='Snapshot')resolve(m.payload);});FSCApp.bridge.send('RequestSnapshot',{});}));
 await demo.close();
 try{
 for(const surface of ['main','mini']){
  const prefs=fs.mkdtempSync(path.resolve('build/review/native-keys-'+surface+'-'));
  fs.mkdirSync(path.join(prefs,'control profiles'));
  const target=path.join(prefs,profile);fs.writeFileSync(target,original);fs.writeFileSync(path.join(prefs,'X-Plane Keys.prf'),original);
  const service=launch(prefs),page=await browser.newPage({viewport:{width:1000,height:850}}),errors=[];
  try{
   page.on('pageerror',e=>errors.push(e.message));
   await page.exposeFunction('nativeKeys',service.request);
   await page.addInitScript(()=>{window.sent=[];window.xplane={fscsKeys:p=>window.nativeKeys(p),fscsPanel:()=>true,fscs:frame=>{
     window.frames=window.frames||new FSCBridge.Assembler();const r=frames.push(frame);
     if(r){sent.push(r);setTimeout(()=>FSCXPlaneReceive(JSON.stringify({type:'ack',requestId:r.requestId,status:'Completed'})),0);}return true;
   }};});
   await page.goto(file+'?surface='+surface+'&host=xplane');
   await page.waitForFunction(()=>window.FSCApp&&window.FSCXPlaneReceive);
   await page.evaluate(s=>FSCBridge.encodeFrames({type:'event',name:'Snapshot',payload:s},'fixture').forEach(FSCXPlaneReceive),snapshot);
   await page.waitForFunction(()=>FSCApp.state.working&&!document.getElementById('mc-camera-binding').disabled);
   assert.equal(await page.locator('#mc-camera-tree .mc-key').count(),0,'no false internal-binding badge');
   async function open(){
    if(surface==='main')await page.locator('#mc-camera-binding').click();
    else{await page.locator('#mc-dock-favorites [data-favorite]').first().click({button:'right'});await page.getByRole('button',{name:'Assign key binding',exact:true}).click();}
    await page.locator('#mc-xp-key-profile option[value="'+profile+'"]').waitFor({state:'attached'});
   }
   await open();
   assert.equal(await page.locator('#mc-xp-key-capture').isDisabled(),true,'profile must be selected explicitly');
   await page.selectOption('#mc-xp-key-profile',profile);
   await page.locator('#mc-xp-key-list').getByRole('button',{name:'Change',exact:true}).click();
   await page.locator('#mc-xp-key-capture').press('Numpad1');
   await page.waitForFunction(()=>!document.getElementById('mc-xp-key-assign').disabled);
   assert.match(await page.locator('#mc-xp-key-preview').innerText(),/sim\/view\/quick_look_1/);
   assert.match(await page.locator('#mc-xp-key-preview').innerText(),/another\/camera/);
   assert.match(await page.locator('#mc-xp-key-preview').innerText(),/replace Numpad-2/);
   for(const height of [850,360]){
    await page.setViewportSize({width:560,height});await page.waitForTimeout(100);
    const b=await page.locator('#mc-xp-key-assign').boundingBox();assert.ok(b.y>=0&&b.y+b.height<=height,'Assign footer visible');
   }
   await page.locator('#mc-xp-key-assign').click();
   await page.waitForFunction(()=>document.getElementById('mc-xp-key-status').textContent.startsWith('Pending restart'));
   assert.equal(fs.readFileSync(target,'utf8'),original,'no native writes while simulator process is running');
   assert.match(await page.locator('#mc-xp-key-list').innerText(),/Numpad-1/);
   assert.doesNotMatch(await page.locator('#mc-xp-key-list').innerText(),/Numpad-2/);
   await page.setViewportSize({width:900,height:850});await page.waitForTimeout(100);
   await page.screenshot({path:path.join(prefs,'assignment-pending.png')});
   // Cancel restores the projected native state without writing the native file.
   await page.locator('#mc-xp-key-cancel').click();
   await page.waitForFunction(()=>document.getElementById('mc-xp-key-status').textContent.startsWith('Native shortcuts'));
   assert.match(await page.locator('#mc-xp-key-list').innerText(),/Numpad-2/);
   // Add an independent modified key, then remove just that key.
   await page.getByRole('button',{name:'Add another key',exact:true}).click();
   await page.locator('#mc-xp-key-capture').press('Control+Alt+2');
   await page.waitForFunction(()=>!document.getElementById('mc-xp-key-assign').disabled);
   await page.locator('#mc-xp-key-assign').click();
   const row=page.locator('.mc-xp-key-row').filter({hasText:'ALT + CTRL + 2'});await row.waitFor();
   await row.getByRole('button',{name:'Remove',exact:true}).click();await row.waitFor({state:'detached'});
   assert.match(await page.locator('#mc-xp-key-list').innerText(),/Numpad-2/);
   await page.locator('#mc-xp-key-cancel').click();
   await page.waitForFunction(()=>document.getElementById('mc-xp-key-status').textContent.startsWith('Native shortcuts'));
   // Final real assignment, with a simulated unrelated native save during shutdown.
   await page.locator('#mc-xp-key-list').getByRole('button',{name:'Change',exact:true}).click();
   await page.locator('#mc-xp-key-capture').press('Numpad1');await page.waitForFunction(()=>!document.getElementById('mc-xp-key-assign').disabled);
   await page.locator('#mc-xp-key-assign').click();await page.waitForFunction(()=>document.getElementById('mc-xp-key-status').textContent.startsWith('Pending restart'));
   await page.getByRole('button',{name:'Done',exact:true}).click();
   const finalSave=original+'Z CTRL sim/late_change\r\n';fs.writeFileSync(target,finalSave);
   await delay(100);assert.equal(fs.readFileSync(target,'utf8'),finalSave);
   assert.deepEqual(errors,[]);
   await page.close();
   service.child.stdin.end();await service.exited;
   const receipt=path.join(prefs,'XPCameraStudio/native-key-result.json');await waitFor(()=>fs.existsSync(receipt));
   const result=JSON.parse(fs.readFileSync(receipt,'utf8'));assert.equal(result.status,'applied',JSON.stringify(result));
   const after=fs.readFileSync(target,'utf8');
   assert.ok(after.includes('Numpad-1 <NONE> xpcs/view_1\r\n'),'actual native XPCS assignment exists');
   assert.ok(!after.includes('sim/view/quick_look_1\r\n')&&!after.includes('another/camera')&&!after.includes('Numpad-2 <NONE>'),'old/conflicting bindings removed');
   assert.ok(after.includes('Numpad-1 CTRL sim/view/quick_look_1_mem\r\n')&&after.includes('Z CTRL sim/late_change\r\n'),'modified and unrelated shortcuts survive');
   assert.equal(fs.readFileSync(result.applied[0].backup,'utf8'),finalSave,'backup is the final native save');
   assert.equal(fs.readFileSync(path.join(prefs,'X-Plane Keys.prf'),'utf8'),original,'only selected profile changes');
   assert.equal(JSON.parse(fs.readFileSync(path.join(prefs,'XPCameraStudio/native-key-queue.json'),'utf8')).operations.length,0);
   console.log('PASS '+surface+': UI capture -> native queue -> real helper waits for process exit -> native reassignment, conflict removal, final-save merge, verified backup.');
  }finally{if(!page.isClosed())await page.close();if(service.child.exitCode===null){service.child.stdin.end();await service.exited;}}
 }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
