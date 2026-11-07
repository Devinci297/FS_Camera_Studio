const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1400,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const id=await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.mode==='aircraftExterior');s.selection.cameraId=c.id;FSCDemo.setOwnership('Observing');return c.id;});
  const cam=id=>page.evaluate(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id),id);
  assert.equal(await page.evaluate(()=>FSCApp.state.caps.modes.free.available),true,'capabilities report free cameras as activatable');
  // View-type picker: Free camera (drone) is offered and places the camera at the current view.
  await page.locator('#mc-tab-behavior').click();
  assert.equal(await page.locator('#mc-free-settings').isHidden(),true,'free settings only for free cameras');
  await page.locator('#mc-view-type').click();
  const modes=await page.locator('#mc-dialog-shade .mc-dialog button[aria-pressed]').allTextContents();
  assert.deepEqual(modes.map(t=>t.trim()),['Cockpit','Aircraft exterior','Orbit','Fly-by','Mounted action cam','Free camera (drone)']);
  await page.getByRole('button',{name:'Free camera (drone)',exact:true}).click();
  await page.waitForFunction(id=>FSCApp.state.working.profile.cameras.find(c=>c.id===id).mode==='free',id,{timeout:3000});
  let saved=await cam(id);
  assert.equal(saved.mode,'free');assert.equal(saved.followAircraft,false);assert.equal(saved.interaction,false);
  assert.deepEqual(saved.worldOrigin,{lat:47.4582,lon:8.5481,altitude:432},'the demo aircraft position anchors the drone');
  assert.deepEqual(saved.positionMetersFRD,[-40,0,-12],'no view on screen: behind and above the aircraft');
  assert.equal(await page.locator('#mc-view-type').innerText(),'Free camera (drone)');
  assert.equal(await page.locator('#mc-view-in-sim').isEnabled(),true);
  // Behavior: the Follow aircraft switch; follow horizon and effects do not apply; glance is not offered.
  await page.locator('#mc-tab-behavior').click();
  assert.equal(await page.locator('#mc-free-settings').isVisible(),true);
  const follow=page.getByRole('checkbox',{name:'Follow aircraft',exact:true});
  assert.equal(await follow.count(),1);assert.equal(await follow.isChecked(),false);
  await follow.check();
  assert.equal((await cam(id)).followAircraft,true);
  assert.match(await page.locator('#mc-free-status').innerText(),/offset from the aircraft/);
  await follow.uncheck();
  assert.equal((await cam(id)).followAircraft,false);
  assert.equal(await page.locator('#mc-follow-horizon-range').isDisabled(),true);
  assert.match(await page.locator('#mc-follow-horizon-note').textContent(),/World-fixed/);
  assert.equal(await page.locator('#mc-glance').isDisabled(),true);
  assert.match(await page.locator('.mc-effective-tracking').first().innerText(),/Free camera - tracks heading \+ pitch - global/);
  await page.locator('#mc-tab-motion').click();
  assert.match(await page.locator('#mc-external-motion').innerText(),/Free camera \(drone\)/);
  assert.equal(await page.locator('#mc-effects-enabled').isDisabled(),true);
  // A live recall works in the demo module; Move drone here then uses the view on screen.
  assert.equal((await page.evaluate(id=>FSCApp.bridge.request('ActivateCamera',{cameraId:id}),id)).status,'Accepted');
  await page.waitForFunction(id=>FSCApp.state.active&&FSCApp.state.active.cameraId===id&&FSCApp.state.ownership.state==='Active',id,{timeout:5000});
  await page.locator('#mc-tab-behavior').click();
  await page.locator('#mc-free-move-here').click();
  await page.waitForTimeout(400);
  saved=await cam(id);assert.deepEqual(saved.worldOrigin,{lat:47.4582,lon:8.5481,altitude:432});
  // Camera library + menu: Drop drone here creates a drone in a Drones category of the aircraft profile and shows it.
  const before=await page.evaluate(()=>FSCApp.state.working.profile.cameras.length);
  await page.locator('#mc-add').click();
  await page.getByRole('button',{name:'Drop drone here',exact:true}).click();
  await page.waitForFunction(n=>FSCApp.state.working.profile.cameras.length===n+1,before,{timeout:3000});
  const dropped=await page.evaluate(()=>{const s=FSCApp.state,p=s.working.profile,c=p.cameras[p.cameras.length-1],g=p.categories.find(g=>g.cameraIds.indexOf(c.id)>=0);return {id:c.id,name:c.name,mode:c.mode,icon:c.icon,follow:c.followAircraft,origin:c.worldOrigin,category:g.name,airport:Boolean(g.airportIcao),selected:s.selection.cameraId===c.id};});
  assert.deepEqual(Object.assign({},dropped,{id:undefined}),{id:undefined,name:'Drone 1',mode:'free',icon:'view-drone',follow:false,origin:{lat:47.4582,lon:8.5481,altitude:432},category:'Drones',airport:false,selected:true});
  await page.waitForFunction(id=>FSCApp.state.active&&FSCApp.state.active.cameraId===id,dropped.id,{timeout:5000});
  // Save and revert keep the drones and their world fields.
  await page.waitForTimeout(500);
  const persisted=await page.evaluate(async ids=>{await FSCApp.bridge.request('CommitChanges',{});await new Promise(r=>setTimeout(r,500));await FSCApp.bridge.request('RevertChanges',{});const p=FSCApp.state.working.profile;return {cams:ids.map(id=>{const c=p.cameras.find(c=>c.id===id);return c&&[c.mode,c.followAircraft,c.worldOrigin.lat];}),dirty:FSCApp.state.working.dirty.profile};},[id,dropped.id]);
  assert.deepEqual(persisted,{cams:[['free',false,47.4582],['free',false,47.4582]],dirty:false});
  // A free camera without a world position is kept but asks to be placed first.
  const unplaced=await page.evaluate(id=>{const c=FSCApp.state.working.profile.cameras.find(c=>c.id===id);const copy=JSON.parse(JSON.stringify(c));delete copy.worldOrigin;return FSCApp.bridge.request('UpdateDraft',{ops:[{op:'setCamera',camera:copy}]}).then(()=>FSCApp.bridge.request('ActivateCamera',{cameraId:id}));},id);
  assert.equal(unplaced.status,'Rejected');assert.match(unplaced.reason,/Drop drone here/);
  assert.deepEqual(errors,[]);
  // Mini panel tools: Drop drone here in the View group.
  const mini=await browser.newPage({viewport:{width:1400,height:420}}),miniErrors=[];mini.on('pageerror',e=>miniErrors.push(e.message));
  await mini.goto(url+'?surface=mini');await mini.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const tool=mini.locator('#mc-mouse-look-drone');
  assert.equal(await tool.count(),1);
  assert.equal(await tool.evaluate(b=>b.closest('.mc-view-controls')!==null),true,'in the View tools');
  assert.equal(await tool.evaluate(b=>b.textContent.trim()),'Drop drone here');assert.match(await tool.getAttribute('title'),/^Drop drone here/);
  const miniBefore=await mini.evaluate(()=>FSCApp.state.working.profile.cameras.filter(c=>c.mode==='free'&&c.worldOrigin).length);
  await tool.evaluate(b=>b.click());
  await mini.waitForFunction(n=>FSCApp.state.working.profile.cameras.filter(c=>c.mode==='free'&&c.worldOrigin).length===n+1,miniBefore,{timeout:3000});
  assert.deepEqual(miniErrors,[]);
  console.log('Free camera view type, Follow aircraft switch, Drop drone here (library and mini), gating and persistence passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
