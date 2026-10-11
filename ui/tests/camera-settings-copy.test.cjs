const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1100,height:800}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  const load=async surface=>{
   await page.goto((surface==='mini'?url.replace(/main\.html$/,'mini.html'):url)+'?surface='+surface);
   await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.waitForTimeout(600);
   await page.evaluate(()=>{
    const s=FSCApp.state,p=s.working.profile;window.calls=[];
    FSCApp.bridge.request=async(action,payload)=>{calls.push({action,payload});return {status:'Completed'};};FSCApp.bridge.send=()=>{};
    const source=p.cameras[0],destination=p.cameras[1];window.sourceId=source.id;window.destinationId=destination.id;
    source.positionMetersFRD=[.8,.2,-.3];source.transition={kind:'smoothstep5',durationSeconds:1.3,delaySeconds:.4};source.mouseLookOnRelease='reset';source.trackingPolicy='off';source.effects={enabled:true,intensity:.7};source.aircraftTracking='both';
    destination.icon='view-instrument';destination.color='#75baff';destination.favorite=true;
    window.identity=JSON.stringify({id:destination.id,name:destination.name,categoryId:destination.categoryId,viewId:destination.viewId,favorite:destination.favorite,icon:destination.icon,color:destination.color,profileDefault:p.defaultCameraId,categories:p.categories,bindings:s.working.bindings});
    s.working.settings.ui.miniHover=false;s.caps.modes.airport={available:true};s.miniCategory=p.categories[0].id;
    document.querySelector('#mc-setting-name-display [data-value="both"]').click();FSCDemo.setOwnership('Observing');
   });
  };
  const select=async id=>page.evaluate(id=>{FSCApp.state.selection.cameraId=window[id];FSCDemo.setOwnership('Observing');},id);
  const menu=async(surface,id)=>{
   await select(id);
   if(surface==='main')await page.locator('#mc-camera-menu').click();
   else {const value=await page.evaluate(id=>window[id],id);await page.locator('#mc-dock-favorites [data-favorite="'+value+'"]').click({button:'right'});}
   await page.waitForTimeout(70);
   assert.ok(await page.locator('#mc-toast').isHidden(),'Old save/copy notification must not cover the menu');const shade=page.locator('#mc-dialog-shade');assert.match(await shade.getAttribute('class'),/mc-context-shade/);
   assert.equal(await shade.evaluate(el=>getComputedStyle(el).backgroundColor),'rgba(0, 0, 0, 0)');
   assert.equal(await shade.evaluate(el=>getComputedStyle(el).pointerEvents),'none');
   const r=await page.locator('.mc-dialog').boundingBox();assert.ok(r.width<=332&&r.x>=0&&r.y>=0&&r.x+r.width<=1101&&r.y+r.height<=801,'Compact menu within viewport');
  };
  await load('main');await page.evaluate(()=>localStorage.removeItem('FSCS.cameraSettingsClipboard.v1'));
  await menu('main','sourceId');assert.ok(await page.getByRole('button',{name:'Paste settings',exact:true}).isDisabled());
  await page.getByRole('button',{name:'Copy settings',exact:true}).click();
  await menu('main','destinationId');await page.getByRole('button',{name:'Paste settings',exact:true}).click();
  assert.ok(await page.locator('[data-paste-setting="pose"]').isEnabled());
  await page.getByRole('button',{name:'Paste settings',exact:true}).click();
  const result=await page.evaluate(()=>{
   const s=FSCApp.state,p=s.working.profile,d=p.cameras.find(c=>c.id===destinationId),source=p.cameras.find(c=>c.id===sourceId);
   return {identity:JSON.stringify({id:d.id,name:d.name,categoryId:d.categoryId,viewId:d.viewId,favorite:d.favorite,icon:d.icon,color:d.color,profileDefault:p.defaultCameraId,categories:p.categories,bindings:s.working.bindings}),expected:identity,position:d.positionMetersFRD,source:source.positionMetersFRD,transition:d.transition,aircraftTracking:d.aircraftTracking,mouse:d.mouseLookOnRelease};
  });
  assert.equal(result.identity,result.expected);assert.deepEqual(result.position,result.source);assert.equal(result.transition.kind,'smoothstep5');assert.equal(result.mouse,'reset');assert.equal(result.aircraftTracking,'off');
  await page.waitForTimeout(130);assert.ok(await page.evaluate(()=>calls.some(c=>c.action==='UpdateDraft'&&c.payload.ops.some(o=>o.op==='setCamera'&&o.camera.id===destinationId))));
  await menu('main','destinationId');await page.locator('.mc-dialog').screenshot({path:'build/review/camera-context-compact-main.png'});await page.locator('#mc-dialog-close').click();
  // A separately loaded mini surface can read the copied settings from the same-origin clipboard.
  await load('mini');await menu('mini','destinationId');assert.ok(await page.getByRole('button',{name:'Paste settings',exact:true}).isEnabled());
  await page.getByRole('button',{name:'Paste settings',exact:true}).click();assert.ok(await page.locator('[data-paste-setting="pose"]').isEnabled());await page.getByRole('button',{name:'Cancel',exact:true}).click();
  await page.evaluate(()=>{
   const p=FSCApp.state.working.profile,s=p.cameras.find(c=>c.id===sourceId),d=p.cameras.find(c=>c.id===destinationId);
   s.mode='airport';s.airportIcao='KAAA';s.worldOrigin={lat:35,lon:-80,altitude:20};
   d.mode='airport';d.airportIcao='KBBB';d.worldOrigin={lat:36,lon:-81,altitude:30};window.destinationPosition=JSON.stringify(d.positionMetersFRD);FSCDemo.setOwnership('Observing');
  });
  await menu('mini','sourceId');await page.getByRole('button',{name:'Copy settings',exact:true}).click();
  await menu('mini','destinationId');await page.getByRole('button',{name:'Paste settings',exact:true}).click();assert.ok(await page.locator('[data-paste-setting="pose"]').isDisabled());
  assert.match(await page.locator('#mc-dialog-body').innerText(),/same airport/);
  await page.getByRole('button',{name:'Paste settings',exact:true}).click();
  assert.equal(await page.evaluate(()=>JSON.stringify(FSCApp.state.working.profile.cameras.find(c=>c.id===destinationId).positionMetersFRD)),await page.evaluate(()=>destinationPosition),'Different airport position preserved');
  await page.evaluate(()=>{const d=FSCApp.state.working.profile.cameras.find(c=>c.id===destinationId);d.mode='aircraftCockpit';delete d.airportIcao;delete d.worldOrigin;FSCDemo.setOwnership('Observing');});
  await menu('mini','destinationId');await page.getByRole('button',{name:'Paste settings',exact:true}).click();assert.ok(await page.locator('[data-paste-setting="pose"]').isDisabled());await page.getByRole('button',{name:'Paste settings',exact:true}).click();
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras.find(c=>c.id===destinationId).aircraftTracking),'off');
  await menu('mini','destinationId');await page.locator('.mc-dialog').screenshot({path:'build/review/camera-context-compact-mini.png'});
  assert.deepEqual(errors,[]);console.log('Compact menus, cross-surface copy/paste, destination identity, native draft path and incompatible pose guards passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
