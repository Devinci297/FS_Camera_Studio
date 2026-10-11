const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright'),fs=require('fs'),path=require('path');
(async()=>{
 const out='doc/manual/images';fs.mkdirSync(out,{recursive:true});
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 const page=await browser.newPage({viewport:{width:1100,height:1000},deviceScaleFactor:1.5});
 await page.goto('file:///'+path.resolve('build/release/0.1.1-test.20261011.1/fs-camera-studio/html_ui/InGamePanels/FSCameraStudio/main.html').replaceAll('\\','/')+'?surface=main');
 await page.waitForFunction(()=>FSCApp.state.snapshot);
 await page.locator('#mc-editor').screenshot({path:out+'/editor.png'});
 for(const [tab,file] of [['mc-tab-behavior','behavior'],['mc-tab-motion','motion']]){
  await page.locator('#'+tab).click();await page.locator('#mc-inspector').screenshot({path:out+'/'+file+'.png'});
 }
 for(const [tab,file] of [['mc-nav-bindings','bindings'],['mc-nav-profiles','profiles'],['mc-nav-settings','settings']]){
  await page.locator('#'+tab).click();await page.locator('#mc-editor').screenshot({path:out+'/'+file+'.png'});
 }
 await page.locator('#mc-app-version').click();await page.locator('#mc-update-settings').screenshot({path:out+'/updates.png'});
 await page.locator('#mc-nav-cameras').click();
 await page.evaluate(()=>{FSCApp.state.airports={loaded:'',nearby:[],facilities:[{icao:'KMCO'},{icao:'KORL'}],radiusNm:25};});
 await page.locator('#mc-add').click();await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();
 await page.locator('#mc-airport-codes').fill('KMCO');
 await page.locator('.mc-dialog').screenshot({path:out+'/airports.png'});
 await browser.close();
 for(const name of ['airport-controls-mini.png','airport-controls-mini-narrow.png','airport-controls-mini-detail.png'])if(fs.existsSync('build/review/'+name))fs.copyFileSync('build/review/'+name,out+'/'+name);
 console.log('Manual UI images captured.');
})().catch(e=>{console.error(e);process.exit(1);});
