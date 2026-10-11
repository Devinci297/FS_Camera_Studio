const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
 const page=await browser.newPage({viewport:{width:1450,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
 assert.match(await page.locator('#mc-movement-hints').innerText(),/Q\s*Up/);assert.match(await page.locator('#mc-movement-hints').innerText(),/E\s*Down/);
 await page.evaluate(()=>{FSCApp.state.working.bindings.bindings.find(b=>b.action==='moveUp').input={kind:'key',definition:'R',label:'R'};FSCDemo.setOwnership('Observing');});
 assert.match(await page.locator('#mc-movement-hints').innerText(),/R\s*Up/);
 for(const key of ['y','pitch']){
  const button=page.locator('[data-axis="'+key+'"] .mc-direction-plus');await button.scrollIntoViewIfNeeded();const r=await button.boundingBox();assert.ok(r.height>=34);
  const before=Number(await page.locator('#mc-pose-'+key).inputValue());await page.mouse.move(r.x+r.width/2,r.y+r.height/2);await page.mouse.down();await page.waitForTimeout(650);await page.mouse.up();
  const after=Number(await page.locator('#mc-pose-'+key).inputValue());assert.ok(after>before+(key==='y'?.02:2));await page.waitForTimeout(250);assert.equal(Number(await page.locator('#mc-pose-'+key).inputValue()),after);
 }
 await page.locator('#mc-save-current-view').click();await page.getByRole('button',{name:'Use these values',exact:true}).click();assert.equal(await page.locator('#mc-save').isEnabled(),true,'Capture leaves draft for explicit saving');
 await page.locator('#mc-save').click();await page.waitForFunction(()=>!FSCApp.state.pendingCommit&&!FSCApp.state.queue.length&&!FSCApp.state.inflight);assert.equal(await page.locator('#mc-save-label').innerText(),'All changes saved');
 for(const width of [1450,850,560]){await page.setViewportSize({width,height:1000});await page.locator('#mc-pose-x').scrollIntoViewIfNeeded();
  const overlap=await page.locator('.mc-direction-control').evaluateAll(ds=>ds.some(d=>{const a=d.querySelector('.mc-direction-minus').getBoundingClientRect(),b=d.querySelector('.mc-direction-plus').getBoundingClientRect();return a.right>b.left+1&&a.bottom>b.top+1;}));assert.equal(overlap,false,'Adjustment buttons do not overlap at '+width);
  await page.screenshot({path:'build/review/ui-editor-refresh-'+width+'.png'});
 }
 assert.deepEqual(errors,[]);console.log('PASS: movement hints follow custom bindings; position and rotation hold/release; draft capture and explicit save; responsive button bounds.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
