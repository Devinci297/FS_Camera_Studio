const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Smart views: the per-camera "Glance view" switch, the three glance binding rows with their default keys, and the
// demo module's hold / next / return including the mini panel's "Glancing" label.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url=surface=>'file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface='+surface;
  const open=async(surface,viewport)=>{
   const page=await browser.newPage({viewport:viewport||{width:1400,height:1000}}),errors=[];
   page.on('pageerror',e=>errors.push(e.message));
   await page.goto(url(surface));
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
   return {page,errors};
  };
  const {page,errors}=await open('main');
  const camera=name=>page.evaluate(n=>{const c=FSCApp.state.working.profile.cameras.find(c=>c.name===n);return c&&c.id;},name);
  const choose=id=>page.evaluate(id=>{FSCApp.state.selection.cameraId=id;FSCDemo.setOwnership('Observing');},id);
  const glanceFlag=id=>page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).glance,id);
  const pfd=await camera('PFD'),pilot=await camera('Pilot view'),tail=await camera('Tail');
  assert.ok(pfd&&pilot&&tail);

  // Defaults: instrument names are glance views, other cockpit views are not.
  assert.equal(await glanceFlag(pfd),true);
  assert.equal(await glanceFlag(pilot),false);
  await choose(pfd);
  await page.locator('#mc-tab-behavior').click();
  const toggle=page.getByRole('checkbox',{name:'Glance view',exact:true});
  assert.equal(await toggle.count(),1);
  assert.equal(await toggle.isChecked(),true);
  assert.equal(await toggle.isEnabled(),true);
  // The switch sits right after "Show in quick access".
  assert.equal(await page.evaluate(()=>document.getElementById('mc-quick-access').closest('label').nextElementSibling.contains(document.getElementById('mc-glance'))),true);
  assert.match(await page.locator('#mc-glance-note').textContent(),/Hold Glance/);
  await choose(pilot);
  assert.equal(await toggle.isChecked(),false);
  await toggle.click();
  assert.equal(await glanceFlag(pilot),true,'switching it on marks the camera');
  await page.waitForTimeout(400);
  await toggle.click();
  assert.equal(await glanceFlag(pilot),false);
  await page.waitForTimeout(400);
  // Exterior views cannot be glance views.
  await choose(tail);
  assert.equal(await toggle.isDisabled(),true);
  assert.equal(await toggle.isChecked(),false);
  assert.match(await page.locator('#mc-glance-note').textContent(),/Only cockpit views/);
  // A new camera without the flag gets it from its name, like an older profile.
  await choose(pilot);
  await page.evaluate(async()=>{
   const s=FSCApp.state,src=s.working.profile.cameras.find(c=>c.name==='Pilot view');
   const cam=Object.assign(JSON.parse(JSON.stringify(src)),{id:crypto.randomUUID(),name:'MFD close-up',viewId:0});delete cam.glance;delete cam.icon;
   const cat=s.working.profile.categories.find(g=>g.cameraIds.indexOf(src.id)>=0);
   cat.cameraIds.push(cam.id);
   await FSCApp.bridge.request('UpdateDraft',{ops:[{op:'setCamera',camera:cam},{op:'setCategories',categories:s.working.profile.categories}]});
   await FSCApp.bridge.request('RequestSnapshot',{});
  });
  await page.waitForFunction(()=>FSCApp.state.working.profile.cameras.some(c=>c.name==='MFD close-up'));
  const added=await camera('MFD close-up');
  assert.equal(await glanceFlag(added),true,'a new instrument camera defaults to a glance view');
  // It sits at the pilot's own pose, so remove it before checking which instrument is nearest.
  await page.evaluate(async id=>{await FSCApp.bridge.request('UpdateDraft',{ops:[{op:'removeCamera',cameraId:id}]});await FSCApp.bridge.request('RequestSnapshot',{});},added);
  await page.waitForFunction(()=>!FSCApp.state.working.profile.cameras.some(c=>c.name==='MFD close-up'));

  // Binding rows, default keys and their own section.
  await page.locator('#mc-nav-bindings').click();
  await page.locator('#mc-binding-search').fill('glance');
  assert.equal(await page.locator('.mc-binding-row').count(),3);
  const expect={glanceHold:['Glance at instrument (hold)','Ctrl + Alt + G'],glanceNext:['Next glance view','Ctrl + Alt + I'],glanceReturn:['Return from glance','Ctrl + Alt + U']};
  for(const [action,[label,key]] of Object.entries(expect)){
   const row=page.locator('[data-binding='+action+']');
   assert.equal(await row.count(),1,action);
   assert.match(await row.innerText(),new RegExp(label.replace(/[()]/g,'\\$&')));
   assert.equal(await row.locator('[data-slot=primary] .mc-binding-input').innerText(),key);
  }
  assert.match(await page.locator('.mc-binding-section summary').first().innerText(),/Glance - instrument close-ups/);
  // No two default inputs collide.
  const defs=await page.evaluate(()=>FSCApp.state.working.bindings.bindings.map(b=>b.input.definition+'|'+(b.input.pov||'')));
  assert.equal(new Set(defs).size,defs.length);

  // Demo module: a held glance (Test presses without a release) shows the nearest instrument; Return goes back.
  const test=(p,action)=>p.evaluate(a=>FSCApp.bridge.request('TestBinding',{binding:{action:a,viewId:0}}),action);
  const activate=async(p,id)=>{await p.evaluate(id=>FSCApp.bridge.request('ActivateCamera',{cameraId:id}),id);await p.waitForFunction(id=>FSCApp.state.ownership.state==='Active'&&FSCApp.state.active.cameraId===id&&!FSCApp.state.active.transitioning,id);};
  await activate(page,pilot);
  assert.equal((await test(page,'glanceHold')).status,'Completed');
  await page.waitForFunction(()=>FSCApp.state.active.glance);
  assert.equal(await page.evaluate(()=>FSCApp.state.active.cameraId),pfd,'looking ahead glances at the PFD');
  assert.equal(await page.evaluate(()=>FSCApp.state.active.glance.returnCameraId),pilot);
  await test(page,'glanceNext');
  await page.waitForFunction(pfd=>FSCApp.state.active.glance&&FSCApp.state.active.cameraId!==pfd,pfd);
  await test(page,'glanceReturn');
  await page.waitForFunction(pilot=>FSCApp.state.active.cameraId===pilot&&!FSCApp.state.active.glance,pilot);
  // A normal recall ends glance mode.
  await test(page,'glanceNext');
  await page.waitForFunction(()=>FSCApp.state.active.glance);
  await activate(page,tail);
  assert.equal(await page.evaluate(()=>Boolean(FSCApp.state.active.glance)),false);
  const reply=await test(page,'glanceHold');
  assert.match(reply.result.message,/cockpit views/);
  assert.deepEqual(errors,[]);

  // Mini panel names the instrument being glanced at.
  const mini=await open('mini',{width:1100,height:260});
  const miniPilot=await mini.page.evaluate(()=>FSCApp.state.working.profile.cameras.find(c=>c.name==='Pilot view').id);
  await activate(mini.page,miniPilot);
  await test(mini.page,'glanceHold');
  await mini.page.waitForFunction(()=>/Glancing/.test(document.getElementById('mc-dock-category').textContent));
  assert.equal(await mini.page.locator('#mc-dock-current').textContent(),'PFD');
  await test(mini.page,'glanceReturn');
  await mini.page.waitForFunction(()=>!/Glancing/.test(document.getElementById('mc-dock-category').textContent));
  assert.equal(await mini.page.locator('#mc-dock-current').textContent(),'Pilot view');
  assert.deepEqual(mini.errors,[]);
  console.log('Glance view switch, defaults, binding rows and keys, demo hold/next/return and mini label passed: '+(process.argv[2]||'source'));
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
