const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1200,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.waitForTimeout(600);
  const before=await page.evaluate(()=>({active:FSCApp.state.working.profile.profileId,profiles:FSCApp.state.profiles}));
  await page.locator('#mc-nav-profiles').click();await page.locator('#mc-export-profile').click();
  const choices=page.locator('#mc-dialog-body .mc-profile-entry');
  assert.equal(await choices.count(),before.profiles.length,'Export lists every saved profile');
  const other=before.profiles.findIndex(p=>p.profileId!==before.active);assert.ok(other>=0,'Fixture has another saved profile');
  await choices.nth(other).getByRole('button',{name:'Export saved',exact:true}).click();
  const code=await page.locator('#mc-dialog-body textarea').inputValue();
  const exported=JSON.parse(code);assert.equal(exported.profileId,before.profiles[other].profileId);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.profileId),before.active,'Export does not switch the open profile');
  await page.locator('#mc-dialog-close').click();
  // Exercise the native asynchronous Pending/CommitCompleted contract; demo normally completes immediately.
  await page.evaluate(()=>{
   const b=FSCApp.bridge,original=b.request.bind(b);b.request=async(a,p)=>{
    const ack=await original(a,p);if(a!=='ImportProfile'||ack.status!=='Completed')return ack;
    const report=ack.result.report;
    ack.result.report={name:report.name,cameraCount:report.cameraCount,aircraft:report.aircraftMatch.displayName,matchesLoadedAircraft:report.aircraftMatch.matches,notes:[],unsupportedCount:0};
    const id='import-roundtrip';FSCApp.state.completedCommits[id]={status:'Committed'};
    return {...ack,status:'Pending',requestId:id};
   };
  });
  await page.locator('#mc-import-profile').click();await page.locator('#mc-dialog-body textarea').fill(code);
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  await page.getByText('Profile imported',{exact:true}).waitFor();
  await page.waitForFunction(n=>FSCApp.state.profiles.length===n+1,before.profiles.length);
  const after=await page.evaluate(()=>FSCApp.state.profiles.map(p=>p.profileId));
  assert.equal(new Set(after).size,after.length,'Imported profile gets a distinct ID');
  await page.locator('#mc-dialog-close').click();await page.locator('#mc-import-profile').click();
  await page.locator('#mc-dialog-body textarea').fill('not a profile');
  await page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  await page.getByText('This profile code is not valid JSON.',{exact:true}).waitFor();
  assert.equal(await page.evaluate(()=>FSCApp.state.profiles.length),after.length);
  assert.deepEqual(errors,[]);console.log('Export selector, unchanged active profile, asynchronous import round-trip and invalid input checks passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
