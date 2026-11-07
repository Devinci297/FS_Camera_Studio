const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const fs=require('fs'),path=require('path'),assert=require('assert/strict');
const A=require('../aircraft-study.js'),W=require('../walkaround.js');
const output=path.resolve('build/aircraft-compat/review',process.argv[2]?path.basename(process.argv[2],'.acf'):'catalog');fs.mkdirSync(output,{recursive:true});
const acf=process.argv[2]?A.parseAcf(fs.readFileSync(process.argv[2],'utf8')):null;
const study=acf?A.fromAcf(acf):{version:1,source:'Test geometry',lengthMeters:20,spanMeters:4,groundDown:3,groundSlope:0,polygons:[[[10,-2,0],[10,2,0],[-10,2,0],[-10,-2,0]]],landmarks:[{name:'Nose',point:[10,0,0],direction:[1,0]},{name:'Tail',point:[-10,0,0],direction:[-1,0]}]};
const expected=W.generate(study);
(async()=>{const browser=await chromium.launch({channel:'msedge',headless:true});try{
 for(const surface of ['main','mini']){
  const page=await browser.newPage({viewport:{width:1500,height:1100}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(__dirname,'../index.html').replaceAll('\\','/')+'?surface='+surface);
  await page.waitForFunction(()=>window.FSCApp?.state.snapshot);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.categories.some(g=>g.name==='Walkaround')),false,'No legacy walkaround in starter profiles');
  await page.evaluate(({acf,study,surface})=>{
   const s=FSCApp.state;
   s.aircraft={title:'Aircraft footprint verification',modelKey:'test-aircraft',variantKey:'test.acf',...(acf?{geometry:{acfProperties:acf}}:{})};
   window.FSCAircraftFootprints={'test-aircraft|test.acf':{...study,modelKey:'test-aircraft',variantKey:'test.acf'}};
   if(surface==='mini'){
    for(const c of s.working.profile.cameras)c.mode='aircraftExterior';
    s.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();
   }
  },{acf,study,surface});
  if(surface==='main'){
   await page.locator('#mc-add').click();await page.getByRole('button',{name:'External motion views',exact:true}).click();
  }else{
   await page.locator('#mc-mouse-look-walkaround').click();
   assert.equal(await page.getByRole('button',{name:'Start walkaround',exact:true}).count(),0,'Ordinary exterior views are not an inspection route');
   await page.getByRole('button',{name:'Study aircraft / add inspection',exact:true}).click();
  }
  const apply=page.getByRole('button',{name:'Add inspection views',exact:true});await apply.waitFor();
  assert.equal(await page.locator('.mc-inspection-stops li').count(),expected.stops.length);
  assert.equal(await apply.isEnabled(),true);
  const transparent=async()=>assert.equal(await page.locator('#mc-dialog-shade').evaluate(e=>getComputedStyle(e).backgroundColor),'rgba(0, 0, 0, 0)');
  await transparent();
  await page.getByLabel('Aircraft clearance (m)',{exact:true}).fill('0');
  assert.equal(await apply.isDisabled(),true);assert.equal(await page.locator('.mc-inspection-map').isVisible(),false);
  await page.getByLabel('Aircraft clearance (m)',{exact:true}).fill('2');
  assert.equal(await apply.isEnabled(),true);assert.equal(await page.locator('.mc-inspection-map').isVisible(),true);
  await page.getByText('Advanced geometry import',{exact:true}).click();
  await page.getByRole('button',{name:'Import aircraft source',exact:true}).click();
  await transparent();await page.screenshot({path:path.join(output,surface+'-import.png')});
  await page.getByRole('button',{name:'Cancel',exact:true}).click();await transparent();
  await page.screenshot({path:path.join(output,surface+'-preview.png')});
  await apply.click();
  const profile=await page.evaluate(()=>FSCApp.state.working.profile),cams=profile.cameras.filter(c=>c.walkaroundPerimeter);
  assert.equal(cams.length,expected.stops.length);
  assert.ok(cams.every(c=>c.walkaroundModelKey==='test-aircraft'&&c.walkaroundPerimeter.length===expected.perimeter.length));
  await page.evaluate(()=>{const s=FSCApp.state,c=s.working.profile.cameras.find(c=>c.walkaroundPerimeter);s.selection.cameraId=c.id;s.selection.categoryId=c.categoryId;FSCDemo.setOwnership('Observing');});
  await page.locator('#'+(surface==='main'?'mc-editor-mouse-look-':'mc-mouse-look-')+'walkaround').click();
  assert.deepEqual(await page.locator('[aria-label="Walkaround category"] button').allTextContents(),['Walkaround inspection']);
  await page.locator('#mc-dialog-close').click();
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(output,surface+'-profile.json'),JSON.stringify(profile));
  await page.close();
 }
 console.log('Automatic aircraft study, main/mini previews, transparent panels, validation recovery and generated profiles passed.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1});
