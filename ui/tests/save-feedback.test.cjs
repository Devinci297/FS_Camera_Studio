const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  const media=await page.evaluate(()=>new Promise(resolve=>{
    const a=new Audio('audio/cameras-saved.wav');
    a.onloadedmetadata=()=>resolve({duration:a.duration,source:a.currentSrc});
    a.onerror=()=>resolve({error:a.error.code});a.load();
  }));
  assert.ok(media.duration>.5&&media.duration<4,'Packaged spoken confirmation decodes');
  const result=await page.evaluate(async()=>{
    let plays=0;window.Audio=function(src){this.src=src;this.pause=()=>{};this.play=()=>{plays++;return Promise.reject(new Error('Playback unavailable'));};};
    FSCSSaveFeedback.play('test-save');FSCSSaveFeedback.play('test-save');FSCSSaveFeedback.play('test-save-2');
    await new Promise(r=>setTimeout(r,20));return plays;
  });
  assert.equal(result,2,'One voice per request; playback denial is contained');assert.deepEqual(errors,[]);
  console.log('Voice WAV decoding, deduplication and playback failure isolation passed.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
