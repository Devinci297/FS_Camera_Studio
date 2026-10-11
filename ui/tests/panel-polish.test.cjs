const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.waitForTimeout(500);
  await page.evaluate(()=>{
   const s=FSCApp.state,p=s.working.profile;
   ['Runways','Gates','Apron','Taxiway intersections','Tower'].forEach(name=>{const g=JSON.parse(JSON.stringify(p.categories[0]));g.id=crypto.randomUUID();g.name=name;g.airportIcao='GOBD';g.icon='map-pin';p.categories.push(g);});
   s.working.settings.ui.nameDisplay='icons';FSCDemo.setOwnership('Observing');
  });
  for(const [name,key] of [['Runways','view-runway'],['Gates','view-gate'],['Apron','view-apron'],['Taxiway intersections','view-taxiway'],['Tower','view-tower']]){
   const button=page.locator('#mc-camera-tree .mc-category-head').filter({hasText:name}).first();
   assert.equal(await button.locator('.mc-entity-name svg').getAttribute('data-icon'),key);
   assert.ok(await button.locator('.mc-entity-label').isVisible(),'Main category labels remain visible');
  }
  await page.locator('#mc-editor-mouse-look').click();
  const dialog=page.locator('.mc-mouselook-shade .mc-dialog');await dialog.waitFor();
  assert.ok((await dialog.boundingBox()).width<=400);
  assert.equal(await page.locator('#mc-dialog-shade').evaluate(el=>getComputedStyle(el).backgroundColor),'rgba(0, 0, 0, 0)');
  await page.getByRole('button',{name:'Return to saved view',exact:true}).click();
  assert.equal(await page.getByRole('button',{name:'Return to saved view',exact:true}).getAttribute('aria-pressed'),'true');
  await page.getByRole('button',{name:'Done',exact:true}).click();
  const plus=page.locator('[data-axis="y"] button').filter({hasText:'Up'});
  const before=Number(await page.locator('#mc-pose-y').inputValue()),r=await plus.boundingBox();
  await page.mouse.move(r.x+r.width/2,r.y+r.height/2);await page.mouse.down();await page.waitForTimeout(720);await page.mouse.up();
  const after=Number(await page.locator('#mc-pose-y').inputValue());assert.ok(after>before+.02,'Held adjustment repeats');
  await page.waitForTimeout(300);assert.equal(Number(await page.locator('#mc-pose-y').inputValue()),after,'Release stops');
  await page.mouse.down();await page.waitForTimeout(420);await page.evaluate(()=>window.dispatchEvent(new Event('blur')));
  const stopped=Number(await page.locator('#mc-pose-y').inputValue());await page.waitForTimeout(300);assert.equal(Number(await page.locator('#mc-pose-y').inputValue()),stopped,'Focus loss stops');await page.mouse.up();
  await page.locator('#mc-add').click();await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();
  assert.equal(await page.locator('.mc-airport-check svg[data-icon="check"]').count(),4);
  assert.equal(await page.locator('.mc-airport-choice-copy strong svg').count(),4);
  await page.locator('#mc-airport-tower').click();assert.equal(await page.locator('#mc-airport-tower').getAttribute('aria-checked'),'false');
  await page.locator('#mc-dialog-close').click();
  const count=await page.evaluate(()=>FSCApp.state.working.profile.cameras.length);
  await page.locator('#mc-add').click();await page.getByRole('button',{name:'External motion views',exact:true}).click();
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras.length),count+8);
  await page.locator('#mc-tab-motion').click();await page.getByRole('spinbutton',{name:'Radius (m)',exact:true}).fill('55');await page.getByRole('spinbutton',{name:'Radius (m)',exact:true}).press('Tab');
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras.find(c=>c.mode==='orbit'&&c.motion).motion.radiusMeters),55);
  assert.equal(await page.getByRole('button',{name:'Walkaround settings',exact:true}).count(),0);
  await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.name==='Walkaround - Nose');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');});
  await page.getByRole('button',{name:'Walkaround settings',exact:true}).click();await page.getByRole('button',{name:'Start walkaround',exact:true}).click();
  await page.waitForFunction(()=>FSCApp.state.walkaround.running);
  assert.deepEqual(errors,[]);await page.screenshot({path:'build/review/panel-polish.png'});
  console.log('PASS: legacy airport icons, main labels, mouse-look popover, hold/release/focus loss, generator SVGs, motion presets and walkaround UI.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
