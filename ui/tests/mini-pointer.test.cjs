const {chromium}=require(process.env.FSCS_PLAYWRIGHT || 'playwright');
const fs=require('node:fs');const assert=require('node:assert/strict');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
 const page=await browser.newPage({viewport:{width:1342,height:1342}});
 const css=fs.readFileSync('package/panel/panel-host.css','utf8');
 await page.setContent(`<style>${css}</style><button id="cockpit" style="position:fixed;inset:0;width:100%;height:100%">Cockpit</button><ingame-ui id="FSCSMini" style="position:fixed;z-index:5"><div class="ingameUiWrapper"><div class="ingameUiContent"><div id="mc-sim"><div class="mc-mini-region" style="position:fixed;bottom:0;height:48px;left:0;right:0"><button id="mini" style="width:100%;height:100%">Mini</button></div></div></div></div></ingame-ui>`);
 await page.evaluate(()=>{window.clicks={cockpit:0,mini:0};document.getElementById('cockpit').onclick=()=>window.clicks.cockpit++;document.getElementById('mini').onclick=()=>window.clicks.mini++;});
 await page.mouse.click(600,20);await page.mouse.click(600,600);await page.mouse.click(600,1320);
 assert.equal(await page.evaluate(()=>window.clicks.cockpit),2,'top/scene clicks pass through the transparent host');assert.equal(await page.evaluate(()=>window.clicks.mini),1,'real mini controls remain clickable');
 await page.evaluate(()=>document.querySelector('.mc-mini-region').style.display='none');await page.mouse.click(600,1320);assert.equal(await page.evaluate(()=>window.clicks.cockpit),3,'disabled mini has no page-sized pointer target');
 console.log('Transparent mini host passes scene and top clicks while preserving mini controls');
}finally{await browser.close()}})().catch(e=>{console.error(e);process.exitCode=1});
