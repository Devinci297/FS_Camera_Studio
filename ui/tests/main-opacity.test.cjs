const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1100,height:950}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.locator('#mc-nav-settings').click();
  assert.equal(await page.locator('#mc-setting-main-opacity').inputValue(),'100');
  assert.equal(await page.locator('.mc-opacity-settings>label').count(),2);
  for(const [id,value] of [['main',35],['mini',15]])await page.evaluate(([id,value])=>{const s=document.getElementById('mc-setting-'+id+'-opacity');s.value=value;s.dispatchEvent(new Event('input',{bubbles:true}));},[id,value]);
  await page.waitForTimeout(350);
  const state=await page.evaluate(()=>({main:getComputedStyle(document.getElementById('mc-editor')).backgroundColor,mini:getComputedStyle(document.getElementById('mc-mini-panel')).backgroundColor,textOpacity:getComputedStyle(document.querySelector('#mc-page-settings label')).opacity,ui:FSCApp.state.working.settings.ui,frames:[...document.querySelectorAll('#mc-editor .mc-global-nav,#mc-editor .mc-browser,#mc-editor .mc-savebar')].map(e=>getComputedStyle(e).backgroundColor)}));
  assert.equal(state.main,'rgba(16, 25, 35, 0.35)');assert.equal(state.mini,'rgba(14, 25, 35, 0.15)');assert.equal(state.textOpacity,'1');
  assert.equal(state.ui.mainOpacity,.35);assert.equal(state.ui.miniOpacity,.15);assert.ok(state.frames.every(x=>x==='rgba(0, 0, 0, 0)'));
  await page.locator('#mc-setting-theme [data-value="light"]').click();
  assert.equal(await page.locator('#mc-editor').evaluate(e=>getComputedStyle(e).backgroundColor),'rgba(246, 248, 251, 0.35)');
  for(const value of [0,100]){
   await page.evaluate(value=>{for(const id of ['main','mini']){const s=document.getElementById('mc-setting-'+id+'-opacity');s.value=value;s.dispatchEvent(new Event('input',{bubbles:true}));}},value);
   await page.waitForTimeout(300);
   const colors=await page.evaluate(()=>['mc-editor','mc-mini-panel'].map(id=>getComputedStyle(document.getElementById(id)).backgroundColor));
   assert.ok(colors.every(color=>value===0?color.endsWith(', 0)'):color.startsWith('rgb(')),'Opacity endpoints must be literal transparent/opaque backgrounds');
  }
  await page.locator('.mc-opacity-settings').screenshot({path:'build/review/main-mini-opacity-controls.png'});
  assert.deepEqual(errors,[]);console.log('Independent adjacent main/mini opacity controls and readable foregrounds passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
