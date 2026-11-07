const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 const out=path.resolve('build/traffic-views/ui');fs.mkdirSync(out,{recursive:true});
 try{
  for(const surface of ['main','mini']){
   const page=await browser.newPage({viewport:{width:1440,height:900}}),errors=[];
   page.on('pageerror',e=>{errors.push(e.message);console.error('PAGE ERROR',e.message);});
   await page.goto('file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>FSCDemo.traffic({time:100}));
   if(surface==='mini'){
    await page.evaluate(()=>document.getElementById('mc-mini-pin').click());
    await page.locator('#mc-dock-traffic-tab').click();
    await page.locator('.mc-dock-traffic-aircraft').first().click();
   }else await page.locator('#mc-nav-traffic').click();
   const container=surface==='mini'?page.locator('.mc-dock-traffic-views'):page.locator('#mc-traffic-list .mc-traffic-row .mc-traffic-views').first();
   const external=page.locator(surface==='mini'?'.mc-dock-traffic-views .mc-traffic-external':'#mc-traffic-list .mc-traffic-row .mc-traffic-external').first();
   let target;
   for(const [view,label] of [['front','Front'],['rear','Rear'],['left','Left side'],['right','Right side'],['overhead','Overhead'],['orbit','Orbit']]){
    if(surface==='main')await page.locator('#mc-nav-traffic').click();
    await external.click();
    assert.equal(await page.locator('.mc-external-spot-choices button').count(),6);
    await page.locator('.mc-external-spot-choices [data-view='+view+']').click();
    await page.waitForFunction(v=>FSCApp.state.spotting.view===v,view);
    const current=await page.evaluate(()=>FSCApp.state.spotting.targetId);if(target===undefined)target=current;else assert.equal(current,target);
    assert.equal(await external.getAttribute('aria-pressed'),'true');
    assert.match(await external.getAttribute('aria-label'),new RegExp(label));
   }
   await container.locator('[data-view=nearest]').click();
   await page.waitForFunction(()=>FSCApp.state.spotting.view==='nearest');
   assert.equal(await page.evaluate(()=>FSCApp.state.spotting.targetId),target);
   assert.equal(await container.locator('[data-view=nearest]').getAttribute('aria-pressed'),'true');
   assert.equal(await external.getAttribute('aria-pressed'),'false');
   assert.ok(await page.evaluate(()=>FSCApp.state.spotting.cameraName.length>0));
   await page.screenshot({path:path.join(out,surface+'-nearest.png')});
   // Track can now hold any world traffic viewpoint, including an external orbit.
   await external.click();await page.locator('.mc-external-spot-choices [data-view=orbit]').click();
   await page.waitForFunction(()=>FSCApp.state.spotting.view==='orbit');
   assert.equal(await container.locator('[data-view=track]').isDisabled(),false);
   await container.locator('[data-view=track]').click();await page.waitForFunction(()=>FSCApp.state.spotting.view==='track');
   for(const width of [1440,900,560,360]){
    await page.setViewportSize({width,height:900});
    await external.click();
    const choices=page.locator('.mc-external-spot-choices button');
    const bounds=await choices.evaluateAll(nodes=>nodes.map(n=>{const b=n.getBoundingClientRect();return{x:b.x,y:b.y,right:b.right,bottom:b.bottom};}));
    assert.ok(bounds.every(b=>b.x>=0&&b.y>=0&&b.right<=width+1&&b.bottom<=900),surface+' buttons fit '+width+': '+JSON.stringify(bounds));
    if(width===1440||width===560)await page.screenshot({path:path.join(out,surface+'-external-'+width+'.png')});
    await page.locator('.mc-external-spot-choices [data-view=front]').click();
    await page.waitForFunction(()=>FSCApp.state.spotting.view==='front');
   }
   assert.deepEqual(errors,[]);await page.close();
  }
  console.log('PASS: main and mini traffic angles, target retention, nearest status, Track, and 360-1440px menus');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
