const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1047,height:980}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.locator('#mc-nav-bindings').click();
  assert.equal(await page.locator('.mc-binding-section[open]').count(),0,'Bindings initially collapsed');
  assert.equal(await page.locator('#mc-toggle-bindings').innerText(),'Expand all');
  await page.locator('#mc-toggle-bindings').click();
  assert.equal(await page.locator('.mc-binding-section[open]').count(),await page.locator('.mc-binding-section').count());
  assert.equal(await page.locator('#mc-toggle-bindings').innerText(),'Collapse all');
  await page.locator('.mc-binding-section summary').first().click();
  await page.waitForFunction(()=>document.getElementById('mc-toggle-bindings').textContent==='Expand all');
  await page.locator('#mc-binding-search').fill('move');
  await page.waitForFunction(()=>document.getElementById('mc-toggle-bindings').textContent==='Collapse all');
  await page.locator('#mc-toggle-bindings').click();
  assert.equal(await page.locator('.mc-binding-section[open]').count(),0);
  for(const width of [750,1047,1440]){
   await page.setViewportSize({width,height:980});
   const boxes=await page.locator('.mc-panel-head').evaluate(h=>[...h.children].filter(e=>e.getBoundingClientRect().width>0).map(e=>({id:e.id||e.className,x:e.getBoundingClientRect().left,right:e.getBoundingClientRect().right,y:e.getBoundingClientRect().top,bottom:e.getBoundingClientRect().bottom})));
   for(let i=0;i<boxes.length;i++)for(let j=i+1;j<boxes.length;j++){
    const a=boxes[i],b=boxes[j];assert.ok(a.right<=b.x+1||b.right<=a.x+1||a.bottom<=b.y+1||b.bottom<=a.y+1,'Header overlaps: '+a.id+' / '+b.id);
   }
   await page.locator('.mc-panel-head').screenshot({path:'build/review/header-polish-'+width+'.png'});
  }
  await page.goto((url.replace(/main\.html$/,'mini.html'))+'?surface=mini');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.waitForTimeout(600);
  await page.evaluate(()=>{
   const s=FSCApp.state,p=s.working.profile;
   for(let i=0;i<12;i++){const g=JSON.parse(JSON.stringify(p.categories[0]));g.id=crypto.randomUUID();g.name='Airport category '+i;g.airportIcao='KAAA';p.categories.push(g);}
   s.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();
  });
  for(const width of [750,1200,1920])for(const mode of ['icons','labels','both']){
   await page.setViewportSize({width,height:980});
   await page.evaluate(mode=>{document.querySelector('#mc-setting-name-display [data-value="'+mode+'"]').click();FSCDemo.setOwnership('Observing');},mode);
   await page.waitForTimeout(600);
   const result=await page.locator('#mc-dock-categories').evaluate(p=>{const r=p.getBoundingClientRect();return {overflow:p.scrollHeight>p.clientHeight+1,clipped:[...p.querySelectorAll('button')].filter(b=>{const t=b.getBoundingClientRect();return t.left<r.left-1||t.right>r.right+1||t.top<r.top-1||t.bottom>r.bottom+1}).map(b=>b.textContent)};});
   assert.equal(result.overflow,false,'Category vertical overflow '+width+'/'+mode);
   assert.deepEqual(result.clipped,[],'Clipped categories '+width+'/'+mode);
   if(width===1200)await page.locator('#mc-mini-panel').screenshot({path:'build/review/categories-'+mode+'.png'});
  }
  assert.deepEqual(errors,[]);console.log('Header bounds, collapsed binding groups, filtered toggle and category visibility passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
