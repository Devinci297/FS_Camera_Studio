// Profiles > Import lists X-Camera files (X-Plane) beside the JSON profiles and converts the chosen one.
const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1200,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.waitForTimeout(600);
  await page.evaluate(()=>{
   window.requests=[];window.xcamera=[];FSCApp.bridge.mode='commbus';
   FSCApp.bridge.request=async(action,payload)=>{
    requests.push({action,payload});
    if(action==='ListImportFiles')return {status:'Completed',result:{files:['Shared.json'],xcamera,folder:'C:\\X-Plane 12\\Output\\preferences\\XPCameraStudio\\imports',nativePicker:false,openFolder:false}};
    if(action==='ImportXCameraFile'){
     FSCApp.state.completedCommits['xcam-test']={status:'Committed'};
     return {status:'Pending',requestId:'xcam-test',result:{report:{name:'ToLiss A319 - X-Camera',cameraCount:44,source:'xcamera',
      notes:['1 camera path that circles the aircraft became an orbit view: it keeps the path\'s distance, height and lap time and starts in front of the aircraft.']}}};
    }
    return {status:'Completed'};
   };
  });
  await page.locator('#mc-nav-profiles').click();await page.locator('#mc-import-profile').click();
  await page.getByRole('button',{name:'Shared.json',exact:true}).waitFor();
  assert.ok(await page.getByText('X-Camera views',{exact:true}).isHidden(),'No X-Camera section without X-Camera files (MSFS)');
  await page.evaluate(()=>xcamera=[
   {id:'x1',label:'X-Camera_a319.csv',aircraft:'ToLissA319_V1p10p1',source:'aircraft',loadedAircraft:true},
   {id:'x2',label:'1436371327_X-Camera_a319.csv',aircraft:'ToLissA319_V1p10p1',source:'community',loadedAircraft:true},
   {id:'x3',label:'X-Camera_CONCORDE_FXP.csv',aircraft:'CONCORDE_FXP',source:'aircraft',loadedAircraft:false}]);
  await page.getByRole('button',{name:'Refresh',exact:true}).click();
  const own=page.getByRole('button',{name:'ToLissA319_V1p10p1 - X-Camera_a319.csv, loaded aircraft',exact:true});
  await own.waitFor();
  assert.ok(await page.getByText('X-Camera views',{exact:true}).isVisible());
  await page.getByRole('button',{name:'ToLissA319_V1p10p1 - 1436371327_X-Camera_a319.csv (community set), loaded aircraft',exact:true}).waitFor();
  // One selection across both lists.
  await page.getByRole('button',{name:'Shared.json',exact:true}).click();
  await own.click();
  assert.equal(await own.getAttribute('aria-pressed'),'true');
  assert.equal(await page.getByRole('button',{name:'Shared.json',exact:true}).getAttribute('aria-pressed'),'false');
  await page.screenshot({path:process.env.FSCS_SCREENSHOT||path.join(require('node:os').tmpdir(),'xcamera-import.png')});
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  await page.getByText('X-Camera views imported',{exact:true}).waitFor();
  assert.deepEqual(await page.evaluate(()=>requests.find(r=>r.action==='ImportXCameraFile').payload),{id:'x1'});
  assert.ok(!(await page.evaluate(()=>requests.some(r=>r.action==='ImportProfileFile'))));
  await page.getByText('How X-Camera\'s cameras were converted:',{exact:true}).waitFor();
  assert.ok(await page.getByText('Kept, but not available in this version:',{exact:true}).isHidden());
  assert.deepEqual(errors,[]);console.log('X-Camera files list beside JSON profiles, share one selection and import with a conversion report.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
