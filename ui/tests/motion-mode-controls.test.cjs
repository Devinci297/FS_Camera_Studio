const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.locator('#mc-add').click();await page.getByRole('button',{name:'External motion views',exact:true}).click();await page.waitForTimeout(600);
  await page.locator('#mc-tab-motion').click();
  assert.equal(await page.getByRole('spinbutton',{name:'Seconds per orbit',exact:true}).count(),1);
  assert.equal(await page.locator('#mc-editor-mouse-look-walkaround').isHidden(),true);assert.equal(await page.getByRole('button',{name:'Walkaround settings',exact:true}).count(),0);
  await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode==='flyby'&&c.motion);s.selection.cameraId=c.id;window.flybyId=c.id;FSCDemo.setOwnership('Observing');});
  assert.match(await page.locator('#mc-external-motion').innerText(),/even while parked/);
  assert.equal(await page.locator('#mc-editor-mouse-look-walkaround').isHidden(),true);assert.equal(await page.getByRole('button',{name:'Walkaround settings',exact:true}).count(),0);
  for(const [name,value] of [['Distance each side (m)','200'],['Seconds per pass','20'],['Height above aircraft (m)','25'],['Side offset (m)','-40']]){const input=page.getByRole('spinbutton',{name,exact:true});await input.fill(value);await input.press('Tab');}
  await page.getByRole('button',{name:'Repeat back and forth: Off',exact:true}).click();assert.match(await page.locator('#mc-external-motion').innerText(),/Average pass speed: 20.0 m\/s/);
  await page.waitForTimeout(800);
  const motion=await page.evaluate(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===flybyId).motion);assert.equal(motion.radiusMeters,200);assert.equal(motion.durationSeconds,20);assert.equal(motion.heightMeters,25);assert.equal(motion.sideMeters,-40);assert.equal(motion.repeat,true);
  const persisted=await page.evaluate(async()=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('RevertChanges',{});return FSCApp.state.working.profile.cameras.find(c=>c.id===flybyId).motion;});assert.deepEqual(persisted,motion);
  await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.name==='Walkaround - Nose');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');});
  await page.locator('#mc-tab-motion').click();await page.getByRole('button',{name:'Walkaround settings',exact:true}).click();const groups=await page.locator('[aria-label="Walkaround category"] button').allTextContents();assert.ok(groups.includes('Walkaround'));assert.ok(!groups.includes('External motion'));await page.locator('#mc-dialog-close').click();
  await page.goto(url.replace(/main\.html$/,'mini.html')+'?surface=mini');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  for(const mode of ['orbit','flyby']){await page.evaluate(mode=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode===mode);s.selection.cameraId=c.id;s.miniCategory=c.categoryId;s.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');},mode);assert.equal(await page.locator('#mc-mouse-look-walkaround').isHidden(),true);}
  assert.deepEqual(errors,[]);console.log('Orbit/flyby mode controls, animated flyby settings persistence, and standalone Walkaround eligibility passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
