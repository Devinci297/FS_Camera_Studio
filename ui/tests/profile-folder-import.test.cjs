const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1200,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.waitForTimeout(600);
  await page.evaluate(()=>{
   window.requests=[];window.files=[];window.failed=false;FSCApp.bridge.mode='commbus';
   FSCApp.bridge.request=async(action,payload)=>{
    requests.push({action,payload});
    if(action==='ListImportFiles')return {status:'Completed',result:{files,folder:'C:\\Test Sim\\WASM\\fs-camera-studio\\work\\imports',nativePicker:false,openFolder:false}};
    if(action==='ImportProfileFile'){
     if(failed)return {status:'Rejected',reason:'That profile file is missing or could not be read.'};
     FSCApp.state.completedCommits['folder-test']={status:'Committed'};
     return {status:'Pending',requestId:'folder-test',result:{report:{name:'Imported file',cameraCount:12}}};
    }
    return {status:'Completed'};
   };
  });
  await page.locator('#mc-nav-profiles').click();await page.locator('#mc-import-profile').click();
  await page.getByText('No JSON files found. Add profiles to this folder, then Refresh.',{exact:true}).waitFor();
  assert.ok(await page.getByRole('button',{name:'Choose profile file',exact:true}).isHidden(),'No nonfunctional browser picker in MSFS');
  assert.match(await page.getByRole('textbox',{name:'Profile imports folder',exact:true}).inputValue(),/work\\imports$/);
  await page.evaluate(()=>files=['Cessna 152.json','A320.json']);await page.getByRole('button',{name:'Refresh',exact:true}).click();
  await page.getByRole('button',{name:'Cessna 152.json',exact:true}).waitFor();
  await page.getByRole('button',{name:'Cessna 152.json',exact:true}).click();
  assert.equal(await page.getByRole('button',{name:'Cessna 152.json',exact:true}).getAttribute('aria-pressed'),'true');
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  await page.getByText('Profile imported',{exact:true}).waitFor();
  assert.deepEqual(await page.evaluate(()=>requests.find(r=>r.action==='ImportProfileFile').payload),{filename:'Cessna 152.json'});
  await page.locator('#mc-dialog-close').click();await page.locator('#mc-import-profile').click();
  await page.getByRole('button',{name:'A320.json',exact:true}).click();await page.evaluate(()=>failed=true);
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  await page.getByText('That profile file is missing or could not be read.',{exact:true}).waitFor();
  assert.ok(await page.getByRole('button',{name:'Refresh',exact:true}).isEnabled());
  await page.getByText('Paste profile code instead',{exact:true}).click();await page.getByRole('textbox',{name:'Profile code',exact:true}).fill('{"fallback":true}');
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  assert.ok(await page.evaluate(()=>requests.some(r=>r.action==='ImportProfile'&&r.payload.code==='{"fallback":true}')));
  assert.deepEqual(errors,[]);console.log('Native folder refresh, file selection/import, missing-file errors, disabled browser picker and paste fallback passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
