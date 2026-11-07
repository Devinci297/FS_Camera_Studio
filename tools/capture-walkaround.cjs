// Capture the inspection dialog for the manual without controlling a simulator.
const fs=require('fs'),path=require('path'),{chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
(async()=>{const browser=await chromium.launch({channel:'msedge',headless:true});try{
 const page=await browser.newPage({viewport:{width:1500,height:1400},deviceScaleFactor:1.5});
 await page.goto('file:///'+path.resolve('package/validated-host/html_ui/InGamePanels/FSCameraStudio/main.html').replaceAll('\\','/')+'?surface=main');
 await page.waitForFunction(()=>window.FSCApp?.state.snapshot);
 if(process.argv[2]){
  const A=require('../ui/aircraft-study.js'),acf=A.parseAcf(fs.readFileSync(process.argv[2],'utf8'));
  await page.evaluate(acf=>{FSCApp.state.aircraft={title:'Example aircraft inspection',modelKey:'manual-example',variantKey:'example.acf',geometry:{acfProperties:acf}};},acf);
 }else await require('../ui/tests/fixtures/inspection.cjs')(page);
 await page.locator('#mc-add').click();await page.getByRole('button',{name:'External motion views',exact:true}).click();
 await page.getByRole('button',{name:'Add inspection views',exact:true}).waitFor();
 if(await page.locator('.mc-inspection-stops li').count()<2)throw Error('Preview has no inspection route');
 await page.evaluate(()=>document.activeElement.blur());
 await page.locator('.mc-inspection-dialog').screenshot({path:'doc/manual/images/walkaround-inspection.png'});
 console.log('Captured aircraft inspection preview.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
