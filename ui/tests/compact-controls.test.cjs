const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1500,height:900}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  for(const surface of ['main','mini']){
   await page.goto((surface==='mini'?url.replace(/main\.html$/,'mini.html'):url)+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>{
    const s=FSCApp.state,p=s.working.profile,c=p.cameras.find(c=>c.mode==='aircraftExterior');s.selection.cameraId=c.id;s.miniCategory=c.categoryId;s.working.settings.ui.miniHover=false;
    window.calls=[];FSCApp.bridge.request=async(a,p)=>{calls.push({a,p});return {status:'Completed'};};FSCApp.bridge.send=()=>{};FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();
   });
   const opener=page.locator('#'+(surface==='mini'?'mc-mouse-look-':'mc-editor-mouse-look-')+'walkaround');
   await opener.click();await page.waitForTimeout(100);
   const shade=page.locator('#mc-dialog-shade'),dialog=shade.locator('.mc-dialog');
   assert.match(await shade.getAttribute('class'),/mc-context-shade/);assert.match(await shade.getAttribute('class'),/mc-walkaround-shade/);
   assert.equal(await shade.evaluate(e=>getComputedStyle(e).backgroundColor),'rgba(0, 0, 0, 0)');assert.equal(await shade.evaluate(e=>getComputedStyle(e).pointerEvents),'none');
   const rect=await dialog.boundingBox();assert.ok(rect.width<=380&&rect.height<460,'Walkaround stays compact');assert.ok(rect.x>=0&&rect.y>=0&&rect.x+rect.width<=1500&&rect.y+rect.height<=900);
   if(surface==='mini'){const dock=await page.locator('#mc-mini-panel').boundingBox();assert.ok(rect.y+rect.height<=dock.y,'Popover sits above dock');fs.mkdirSync('build/review',{recursive:true});await page.screenshot({path:'build/review/walkaround-compact-packaged.png'});}
   assert.equal(await page.locator('#mc-dialog-actions .mc-primary').innerText(),'Start walkaround');
   await page.getByRole('button',{name:'Repeat: On',exact:true}).click();assert.equal(await page.getByRole('button',{name:'Repeat: Off',exact:true}).getAttribute('aria-pressed'),'false');
   await page.getByRole('spinbutton',{name:'Seconds at each camera'}).fill('7');await page.getByRole('button',{name:'Start walkaround',exact:true}).click();
   const start=await page.evaluate(()=>calls.find(c=>c.a==='StartWalkaround'));assert.equal(start.p.intervalSeconds,7);assert.equal(start.p.loop,false);
   await page.evaluate(()=>{FSCApp.state.walkaround.running=true;FSCDemo.setOwnership('Observing');});await opener.click();assert.equal(await page.locator('#mc-dialog-actions .mc-primary').innerText(),'Stop walkaround');await page.getByRole('button',{name:'Stop walkaround',exact:true}).click();assert.equal(await page.evaluate(()=>calls.some(c=>c.a==='StopWalkaround')),true);
   if(surface==='main')await page.locator('#mc-camera-menu').click();else await page.locator('#mc-dock-favorites button').first().click({button:'right'});
   await page.getByRole('button',{name:'Delete camera',exact:true}).click();await page.waitForTimeout(100);
   assert.match(await shade.getAttribute('class'),/mc-confirm-shade/);assert.equal(await shade.evaluate(e=>getComputedStyle(e).backgroundColor),'rgba(0, 0, 0, 0)');assert.equal(await shade.evaluate(e=>getComputedStyle(e).pointerEvents),surface==='main'?'auto':'none');
   assert.ok((await dialog.boundingBox()).height<250);assert.equal(await page.getByRole('button',{name:'Cancel',exact:true}).count(),1);assert.equal(await page.getByRole('button',{name:'Delete',exact:true}).count(),1);
   if(surface==='main'){await page.screenshot({path:'build/review/delete-compact-packaged.png'});const hit=await page.evaluate(()=>{const b=document.getElementById('mc-nav-settings').getBoundingClientRect();return document.elementFromPoint(b.x+b.width/2,b.y+b.height/2).id;});assert.equal(hit,'mc-dialog-shade','Confirmation blocks main editor background actions');}
   await page.getByRole('button',{name:'Cancel',exact:true}).click();
  }
  assert.deepEqual(errors,[]);console.log('Packaged compact Walkaround and confirmation layout, transparent shade, main blocking, timing/repeat/start/stop controls passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
