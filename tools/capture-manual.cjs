const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright'),fs=require('fs'),path=require('path');
(async()=>{
 const out='doc/manual/images';fs.mkdirSync(out,{recursive:true});
 const source=path.resolve(process.argv[2]||'package/validated-host/html_ui/InGamePanels/FSCameraStudio/main.html');
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
 const page=await browser.newPage({viewport:{width:1250,height:1000},deviceScaleFactor:1.5});
 await page.goto('file:///'+source.replaceAll('\\','/')+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
 await page.evaluate(()=>{const s=FSCApp.state;s.working.profile.categories.forEach(g=>s.expanded[g.id]=true);FSCDemo.setOwnership('Observing');});
 const shot=async(file,selector='#mc-editor')=>{await page.evaluate(()=>document.activeElement&&document.activeElement.blur());await page.mouse.move(1240,20);await page.waitForTimeout(400);await page.locator(selector).screenshot({path:out+'/'+file+'.png'});};
 await shot('editor');await shot('library','.mc-browser');
 for(const [tab,file] of [['mc-tab-behavior','behavior'],['mc-tab-motion','motion']]){await page.locator('#'+tab).click();await shot(file,'#mc-inspector');}
 await page.locator('#mc-camera-menu').click();await page.getByRole('button',{name:'Assign key binding',exact:true}).click();
 await page.locator('[data-slot="secondary"]').first().click();await page.locator('#mc-assign-device').click();await page.locator('#mc-controller-choices [data-device-id="1"]').click();await page.locator('#mc-assign-listen').click();await page.waitForFunction(()=>FSCApp.state.assign.listening);await shot('controller-listening','.mc-dialog');
 await page.evaluate(()=>{FSCDemo.captureControllerHat(1,0);FSCDemo.captureControllerHat(1,4294967295);});await page.waitForFunction(()=>FSCApp.state.assign.staged&&FSCApp.state.assign.staged.pov);await shot('controller-hat','.mc-dialog');await page.getByRole('button',{name:'Cancel',exact:true}).click();
 await page.locator('#mc-nav-bindings').click();await page.locator('#mc-bindings-list summary').first().click();await shot('bindings');
 await page.locator('#mc-nav-profiles').click();await shot('profiles');await page.locator('#mc-export-profile').click();await shot('export-picker','.mc-dialog');await page.locator('#mc-dialog-close').click();
 await page.evaluate(()=>{window.manualRequest=FSCApp.bridge.request.bind(FSCApp.bridge);FSCApp.bridge.mode='commbus';FSCApp.bridge.request=async(action,payload)=>action==='ListImportFiles'?{status:'Completed',result:{files:['Cessna-152-Test-Profile.json','My-shared-profile.json'],folder:'%APPDATA%\\Microsoft Flight Simulator 2024\\WASM\\MSFS2024\\fs-camera-studio\\work\\imports'}}:window.manualRequest(action,payload);});
 await page.locator('#mc-import-profile').click();await page.getByRole('button',{name:'Cessna-152-Test-Profile.json',exact:true}).waitFor();await shot('import-profile','.mc-dialog');await page.locator('#mc-dialog-close').click();await page.evaluate(()=>{FSCApp.bridge.mode='demo';FSCApp.bridge.request=window.manualRequest;});
 await page.locator('#mc-nav-settings').click();await shot('settings');
 await page.setViewportSize({width:1250,height:1500});
 for(const section of ['tracking','airport','updates']){await page.locator('#mc-settings-link-'+section).click();await shot(section==='updates'?'updates':section+'-settings','#mc-settings-section-'+section);}
 await page.setViewportSize({width:1250,height:1000});
 await page.locator('#mc-nav-cameras').click();await page.evaluate(()=>{FSCApp.state.airports={loaded:'',nearby:[],facilities:[{icao:'KMCO'},{icao:'KORL'}],radiusNm:25};});
 await page.locator('#mc-add').click();await page.getByRole('button',{name:'Create airport cameras',exact:true}).click();await page.locator('#mc-airport-codes').fill('KMCO');await shot('airports','.mc-dialog');
 const mini=await browser.newPage({viewport:{width:1600,height:400},deviceScaleFactor:1.5});await mini.goto('file:///'+source.replace(/main\.html$/,'mini.html').replaceAll('\\','/')+'?surface=mini');await mini.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
 await mini.evaluate(()=>{FSCApp.state.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();});
 for(const [width,file] of [[1600,'airport-controls-mini'],[1200,'airport-controls-mini-detail'],[760,'airport-controls-mini-narrow']]){await mini.setViewportSize({width,height:400});await mini.waitForTimeout(100);await mini.locator('#mc-mini-panel').screenshot({path:out+'/'+file+'.png'});}
 fs.writeFileSync(out+'/capture.json',JSON.stringify({version:await page.evaluate(()=>FSCS_BUILD_VERSION),source:path.relative(process.cwd(),source),environment:'Browser demo fixtures; not a live simulator capture',captured:new Date().toISOString()},null,2)+'\n');
 console.log('Captured current release UI images.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
