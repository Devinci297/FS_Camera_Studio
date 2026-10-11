const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  for(const surface of ['main','mini']){
   const page=await browser.newPage({viewport:{width:1450,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
   const url=path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
   await page.goto('file:///'+(surface==='mini'?url.replace(/main\.html$/,'mini.html'):url)+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   const ids=await page.evaluate(()=>{
    const s=FSCApp.state,p=s.working.profile,g=p.categories[0];s.working.settings.ui.miniHover=false;
    const cams=g.cameraIds.map(id=>p.cameras.find(c=>c.id===id));cams.forEach(c=>c.favorite=false);cams[1].favorite=true;cams[2].favorite=true;cams[1].viewId=0;
    s.selection.cameraId=cams[1].id;s.miniCategory=g.id;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();
    return {target:cams[1].id,next:cams[2].id,hidden:cams[0].id,group:g.id};
   });
   if(surface==='mini'){
    assert.deepEqual(await page.locator('#mc-dock-favorites [data-favorite]').evaluateAll(bs=>bs.map(b=>b.dataset.favorite)),[ids.target,ids.next]);
    await page.locator('#mc-dock-categories [data-focus-key="category:'+ids.group+'"]').click();
    await page.waitForFunction(id=>FSCApp.state.active.cameraId===id,ids.target);
    await page.locator('#mc-next').click();await page.waitForFunction(id=>FSCApp.state.active.cameraId===id,ids.next);
    await page.locator('#mc-next').click();await page.waitForFunction(id=>FSCApp.state.active.cameraId===id,ids.target);
    await page.locator('#mc-dock-favorites [data-favorite="'+ids.target+'"]').click({button:'right'});
   }else await page.locator('#mc-camera-menu').click();
   await page.getByRole('button',{name:'Assign key binding',exact:true}).click();
   await page.locator('#mc-assign-capture').press('Control+Alt+F10');
   await page.locator('#mc-dialog-actions').getByRole('button',{name:'Assign',exact:true}).click();
   await page.waitForFunction(()=>FSCApp.state.working.bindings.bindings.some(b=>b.input.definition==='VK_LCONTROL+VK_LMENU+VK_F10'));
   const assigned=await page.evaluate(id=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.id===id);return {id:c.viewId,b:s.working.bindings.bindings.find(b=>b.input.definition==='VK_LCONTROL+VK_LMENU+VK_F10')};},ids.target);
   assert.ok(assigned.id>0);assert.equal(assigned.b.viewId,assigned.id);
   if(surface==='mini'){
    await page.locator('#mc-dock-favorites [data-favorite="'+ids.target+'"]').click({button:'right'});
    await page.getByRole('button',{name:'Remove star (hide in mini panel)',exact:true}).click();
    assert.equal(await page.locator('#mc-dock-favorites [data-favorite="'+ids.target+'"]').count(),0);
    await page.locator('#mc-dock-favorites [data-favorite="'+ids.next+'"]').click({button:'right'});
    await page.getByRole('button',{name:'Remove star (hide in mini panel)',exact:true}).click();
    assert.equal(await page.locator('#mc-dock-favorites [data-favorite]').count(),0);
    assert.ok(await page.locator('#mc-next').isDisabled());
    assert.match(await page.locator('#mc-dock-favorites').innerText(),/No starred cameras/);
    await page.locator('#mc-mini-panel').screenshot({path:'build/review/starred-mini-empty.png'});
   }
   assert.deepEqual(errors,[]);await page.close();
  }
  console.log('Main and mini context key assignment, starred-only display/category selection/cycling, unstar and empty state passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
