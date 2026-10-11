const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path'),fs=require('node:fs');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1200,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.waitForTimeout(600);
  await page.locator('#mc-nav-profiles').click();
  await page.evaluate(()=>{
   window.imports=[];FSCApp.bridge.request=async(action,payload)=>{
    if(action!=='ImportProfile')return {status:'Completed'};
    imports.push(payload.code);
    const id='file-import';FSCApp.state.completedCommits[id]={status:'Committed'};
    return {status:'Pending',requestId:id,result:{report:{name:'Cessna 152 test',cameraCount:12}}};
   };
  });
  const open=async()=>page.locator('#mc-import-profile').click();
  const submit=()=>page.locator('#mc-dialog-actions').getByRole('button',{name:'Import',exact:true}).click();
  const select=payload=>page.locator('input[type=file]').setInputFiles(payload);
  await open();assert.ok(await page.getByRole('button',{name:'Choose profile file',exact:true}).isVisible());
  assert.ok(await page.getByRole('textbox',{name:'Profile code',exact:true}).isHidden(),'Paste is secondary');
  const chooser=page.waitForEvent('filechooser');await page.getByRole('button',{name:'Choose profile file',exact:true}).click();
  await (await chooser).setFiles(path.resolve('profiles/cessna-152-test.json'));
  await page.getByRole('status').filter({hasText:'cessna-152-test.json'}).waitFor();await submit();
  await page.getByText('Profile imported',{exact:true}).waitFor();
  assert.equal(await page.evaluate(()=>imports.length),1);
  assert.deepEqual(JSON.parse(await page.evaluate(()=>imports[0])),JSON.parse(fs.readFileSync('profiles/cessna-152-test.json','utf8')));
  await page.locator('#mc-dialog-close').click();await open();
  await select({name:'empty.json',mimeType:'application/json',buffer:Buffer.alloc(0)});
  await page.getByText('This profile file is empty. Choose another file.',{exact:true}).waitFor();await submit();
  assert.equal(await page.evaluate(()=>imports.length),1,'Empty file never submitted');
  await select({name:'too-large.json',mimeType:'application/json',buffer:Buffer.alloc(10*1024*1024+1)});
  await page.getByText('The profile file is larger than 10 MiB.',{exact:true}).waitFor();
  assert.equal(await page.evaluate(()=>imports.length),1,'Oversize file never submitted');
  // Choosing a file, then switching to pasted code, imports only the pasted selection.
  await select({name:'old.json',mimeType:'application/json',buffer:Buffer.from('{"old":true}')});
  await page.getByRole('status').filter({hasText:'old.json'}).waitFor();
  await page.getByText('Paste profile code instead',{exact:true}).click();
  await page.getByRole('textbox',{name:'Profile code',exact:true}).fill('{"new":true}');await submit();
  await page.getByText('Profile imported',{exact:true}).waitFor();assert.equal(await page.evaluate(()=>imports[1]),'{"new":true}');
  await page.locator('#mc-dialog-close').click();
  await page.evaluate(()=>{window.originalReader=window.FileReader;window.FileReader=undefined;});
  await open();assert.ok(await page.getByRole('button',{name:'Choose profile file',exact:true}).isDisabled());
  assert.ok(await page.getByRole('textbox',{name:'Profile code',exact:true}).isVisible());
  assert.match(await page.locator('#mc-dialog-body').innerText(),/File selection is unavailable/);
  assert.deepEqual(errors,[]);
  console.log('File picker, JSON reading, asynchronous import, empty/oversize guards, pasted override and unavailable-API fallback passed. MSFS picker support remains a live check.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
