const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.FSCS_BROWSER,headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1200,height:850}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));page.setDefaultTimeout(60000);
  await page.goto('file:///'+path.resolve('ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const transport=await page.evaluate(()=>{
    const message={type:'event',payload:{data:'x'.repeat(5*1024*1024)}},frames=FSCBridge.encodeFrames(message,'large');
    const assembler=new FSCBridge.Assembler();let got;
    frames.forEach(f=>{got=assembler.push(f);});
    return {count:frames.length,length:got&&got.payload.data.length};
  });
  assert.ok(transport.count>1024);assert.equal(transport.length,5*1024*1024);
  async function load(count){
   const result=await page.evaluate(async count=>{
    const p=JSON.parse(JSON.stringify(FSCApp.state.working.profile)),base=p.cameras[0],group=p.categories[0];
    group.cameraIds=[];delete group.defaultCameraId;p.cameras=[];p.categories=[group];
    for(let i=0;i<count;i++){const c=JSON.parse(JSON.stringify(base));c.id=FSCMath.uuid();c.name='Camera '+(i+1);c.viewId=0;c.categoryId=group.id;c.favorite=false;p.cameras.push(c);group.cameraIds.push(c.id);}
    p.defaultCameraId=p.cameras[0].id;p.name='Capacity '+count;
    const result=await FSCApp.bridge.request('ImportProfile',{code:JSON.stringify(p)});
    if(result.status!=='Completed')return result;
    await FSCApp.bridge.request('OpenProfile',{profileId:result.result.profileId,discard:true});
    return result;
   },count);
   assert.equal(result.status,'Completed',JSON.stringify(result));
   await page.waitForFunction(n=>FSCApp.state.working.profile.cameras.length===n,count);
  }
  for(const count of [500,1999]){
   await load(count);
   await page.locator('#mc-add').click();await page.getByRole('button',{name:'Drop drone here',exact:true}).click();
   await page.waitForFunction(n=>FSCApp.state.working.profile.cameras.length===n+1,count);
   await page.waitForFunction(()=>!FSCApp.state.inflight&&!FSCApp.state.queue.length);
   const ack=await page.evaluate(()=>FSCApp.bridge.request('CommitChanges',{}));
   assert.ok(['Pending','Completed'].includes(ack.status));
   await page.waitForFunction(()=>!FSCApp.state.pendingCommit&&!FSCApp.state.working.dirty.profile);
   await page.evaluate(()=>FSCApp.bridge.request('RevertChanges',{}));
   await page.waitForFunction(n=>FSCApp.state.working.profile.cameras.length===n+1,count);
  }
  await page.locator('#mc-add').click();await page.getByRole('button',{name:'Drop drone here',exact:true}).click();
  assert.match(await page.locator('#mc-toast').innerText(),/2,000 cameras/);
  assert.equal(await page.evaluate(()=>FSCApp.state.working.profile.cameras.length),2000);
  assert.deepEqual(errors,[]);
  console.log('PASS: 500 -> 501 and 1,999 -> 2,000 cameras, save/reload, explicit 2,000 cap, and >1,024-frame / 5 MiB panel messages.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
