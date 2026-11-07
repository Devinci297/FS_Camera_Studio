const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
const base='https://github.com/Devinci297/FS_Camera_Studio';
const release=v=>({tag_name:'v'+v,draft:false,prerelease:false,assets:[{name:`FS-Camera-Studio-${v}-Setup.exe`,browser_download_url:`${base}/releases/download/v${v}/FS-Camera-Studio-${v}-Setup.exe`,size:100,state:'uploaded'}]});
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.FSCS_BROWSER,headless:true});
 fs.mkdirSync('build/review/updates',{recursive:true});
 try{
  for(const surface of ['main','mini']){
   const page=await browser.newPage({viewport:{width:1100,height:surface==='main'?850:400}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
   let requests=0,response=[];await page.route('https://api.github.com/**',route=>{requests++;return route.fulfill({status:200,contentType:'application/json',headers:{'Access-Control-Allow-Origin':'*'},body:JSON.stringify(response)});});
   const url='file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface='+surface;
   await page.goto(url);await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   const current=await page.evaluate(()=>FSCS_BUILD_VERSION),parts=current.split('.').map(Number);parts[2]++;const next=parts.join('.');response=[release(next)];
   const snapshot=await page.evaluate(()=>new Promise(resolve=>{FSCApp.bridge.on('event',m=>{if(m.name==='Snapshot')resolve(m.payload);});FSCApp.bridge.send('RequestSnapshot',{});}));
   await page.evaluate(()=>{FSCApp.bridge.demo=null;window.originalNow=Date.now;window.advance=0;Date.now=()=>originalNow()+advance;FSCSUpdates.maybeCheck();});
   await page.waitForFunction(v=>document.getElementById('mc-update-status').textContent==='Update available: '+v,next);
   assert.equal(requests,1,'automatic first check without opening Settings');
   if(surface==='main'){
    assert.equal(await page.locator('#mc-update-notice').isVisible(),true);
    await page.screenshot({path:'build/review/updates/main-notice.png'});
    await page.locator('#mc-update-later').click();assert.equal(await page.locator('#mc-update-notice').isVisible(),false);
    await page.reload();await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
    assert.equal(await page.locator('#mc-update-notice').isVisible(),false,'dismissal survives reload');
    assert.equal(await page.locator('#mc-app-version').innerText(),'v'+current+' - Update','badge keeps known update');
    await page.locator('#mc-app-version').click();await page.locator('#mc-update-check').click();
    await page.waitForFunction(()=>!document.getElementById('mc-update-check').disabled);
    assert.equal(await page.locator('#mc-update-notice').isVisible(),true,'explicit check restores notification');
    await page.evaluate(()=>{window.OpenBrowser=url=>window.updateRequested=url;});
    assert.equal(await page.locator('#mc-update-download').isVisible(),false,'manual download is hidden under troubleshooting');
    await page.locator('#mc-update-install').click();assert.equal(await page.evaluate(()=>updateRequested),'fscamerastudio://update/'+next);
    await page.locator('#mc-update-auto').uncheck();const before=requests;
    await page.evaluate(()=>{FSCApp.bridge.demo=null;FSCSUpdates.maybeCheck();});assert.equal(requests,before,'opt out');
    await page.screenshot({path:'build/review/updates/main-settings.png'});
   }else{
    await page.evaluate(()=>{document.getElementById('mc-mini-pin').click();});
    assert.equal(await page.locator('#mc-mini-update').isVisible(),true,'mini-only notification');
    await page.evaluate(()=>{document.body.style.background='#23333e';document.getElementById('mc-mini-panel').style.background='#152330';});
    await page.waitForTimeout(300);await page.screenshot({path:'build/review/updates/mini-notice.png'});
    await page.setViewportSize({width:360,height:400});await page.waitForTimeout(100);
    const bounds=await page.locator('#mc-mini-update').boundingBox();assert.ok(bounds&&bounds.x>=0&&bounds.x+bounds.width<=360,'update fits narrow mini');
    await page.screenshot({path:'build/review/updates/mini-narrow.png'});
    await page.setViewportSize({width:1100,height:400});
   }
   // Native XP host calls receive structured results. No real Windows process is launched by this UI fixture.
   await page.addInitScript(()=>{window.launches=[];window.xplane={fscs:()=>true,fscsPanel:()=>true,fscsUpdate:v=>{launches.push(v);return Promise.resolve({ok:true});},fscsOpenUrl:url=>Promise.resolve(true)};});
   await page.goto(url+'&host=xplane');await page.waitForFunction(()=>window.FSCApp&&window.FSCXPlaneReceive);
   await page.evaluate(s=>FSCBridge.encodeFrames({type:'event',name:'Snapshot',payload:s},'fixture').forEach(FSCXPlaneReceive),snapshot);
   await page.evaluate(()=>{localStorage.removeItem('fscs.updateLease');FSCSUpdates.check();});
   await page.waitForFunction(v=>document.getElementById('mc-update-status').textContent==='Update available: '+v,next);
   if(surface==='mini')await page.evaluate(()=>document.getElementById('mc-mini-pin').click());
   await page.locator(surface==='main'?'#mc-update-notice-install':'#mc-mini-update').click();
   assert.deepEqual(await page.evaluate(()=>launches),[next]);
   await page.evaluate(()=>{xplane.fscsUpdate=()=>Promise.resolve({ok:false,error:'Updater missing'});});
   await page.locator(surface==='main'?'#mc-update-notice-install':'#mc-mini-update').click();
   assert.match(await page.locator('#mc-update-status').textContent(),/Updater missing.*Download/);
   assert.equal(await page.locator('.mc-update-fallback').getAttribute('open')!==null,true,'failure exposes recovery controls');
   // Time passage exercises repeat scheduling without another six-hour wait.
   const before=requests;
   await page.evaluate(()=>{window.realNow=Date.now;Date.now=()=>realNow()+7*60*60*1000;document.getElementById('mc-update-auto').checked=true;localStorage.removeItem('fscs.updateLease');FSCSUpdates.maybeCheck();});
   await page.waitForFunction(()=>!document.getElementById('mc-update-check').disabled);assert.equal(requests,before+1,'periodic recheck');
   await page.evaluate(()=>FSCSUpdates.maybeCheck());assert.equal(requests,before+1,'no request spam');
   assert.deepEqual(errors,[]);await page.close();
  }
  console.log('PASS: automatic checks, persisted notification/dismissal, opt-out, periodic retry, mini-only notification, MSFS protocol and native XPCS updater handoff.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
