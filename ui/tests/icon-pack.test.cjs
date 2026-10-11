const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'build/review/readable-icons-import/fs-camera-studio/html_ui/InGamePanels/FSCameraStudio/main.html').replaceAll('\\','/');
  for(const surface of ['main','mini']){
   await page.goto((surface==='main'?url:url.replace(/main\.html$/,'mini.html'))+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   await page.evaluate(()=>{
    const s=FSCApp.state;
    s.working.settings.ui.miniHover=false;
    FSCDemo.setOwnership('Observing');
   });
   await page.waitForTimeout(400);
   const result=await page.evaluate(async()=>{
    const names=FSCIcons.pickerNames;
    return {count:names.length,all:names.every(n=>{const icon=FSCIcons.icon(n);return icon.children.length>0&&!icon.querySelector('image')&&icon.getAttribute('stroke')==='currentColor';}),
     visible:[...document.querySelectorAll('svg.mc-icon')].filter(e=>e.getBoundingClientRect().width&&e.getBoundingClientRect().height).length};
   });
   assert.equal(result.count,36);assert.ok(result.all);assert.ok(result.visible>5);
   if(surface==='main'){
    await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras[0];c.color='#75baff';s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');});
    await page.screenshot({path:'build/review/icon-integration-main.png'});
    await page.locator('#mc-camera-icon').click();
    assert.equal(await page.locator('[data-icon-choice]').count(),39);
    await page.screenshot({path:'build/review/icon-integration-picker.png'});
    await page.locator('[data-icon-choice="view-tower"]').click();
    assert.equal(await page.locator('#mc-camera-icon > svg').getAttribute('data-icon'),'view-tower');
    assert.equal(await page.locator('#mc-camera-icon > svg').evaluate(el=>getComputedStyle(el).color),'rgb(117, 186, 255)');
    assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras[0].icon),'view-tower');
   }else{
    await page.locator('#mc-mini-pin').evaluate(el=>el.click());
    await page.locator('#mc-mini-panel').screenshot({path:'build/review/icon-integration-mini.png'});
    assert.equal(await page.locator('#mc-mouse-look-save > svg').getAttribute('data-icon'),'save');
    assert.equal(await page.locator('#mc-mouse-look-head > svg').getAttribute('data-icon'),'head-tracking');
    const camera=page.locator('#mc-dock-favorites [data-favorite]').first();
    await camera.hover();await page.waitForTimeout(550);
    assert.ok(await page.locator('#mc-control-tooltip').isHidden(),'Labeled camera has no tooltip');
    assert.equal(await camera.getAttribute('title'),null,'No duplicate browser title');
    await page.mouse.move(2,2);
    await camera.locator('.mc-entity-label').evaluate(el=>el.style.display='none');
    await camera.hover();await page.waitForTimeout(550);
    assert.ok(await page.locator('#mc-control-tooltip').isVisible(),'Icon-only camera exposes its name');
    assert.equal(await page.locator('#mc-control-tooltip').textContent(),await camera.getAttribute('data-camera-name'));
   }
  }
  assert.deepEqual(errors,[]);console.log('PASS: 36 simple SVG icons; picker selection; exact color; main and mini controls; camera tooltip visibility.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
