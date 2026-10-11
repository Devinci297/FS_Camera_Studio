const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright'),assert=require('node:assert/strict'),path=require('node:path');
(async()=>{const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});try{
 const page=await browser.newPage({viewport:{width:1280,height:900}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=main');await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
 const ids=await page.evaluate(()=>{const s=FSCApp.state,p=s.working.profile,first=p.categories[0],cam=p.cameras[0];
  first.name='Cockpit';first.cameraIds=first.cameraIds.slice(0,5);p.categories=p.categories.slice(0,3);p.categories.forEach(g=>s.expanded[g.id]=false);s.expanded[first.id]=true;s.airportExpanded=true;
  ['Runways','Gates','Apron','Taxiway intersections','Tower'].forEach((name,i)=>{const id='layout-airport-'+i,g={...first,id,name,airportIcao:'GOBD',cameraIds:[]};for(let n=0;n<(i===2?80:i===3?75:5);n++){const c={...cam,id:id+'-'+n,name:name+' camera '+n,categoryId:id,airportIcao:'GOBD',mode:'airport',viewId:0};p.cameras.push(c);g.cameraIds.push(c.id);}p.categories.push(g);});
  cam.name='Pilot - normal';s.working.bindings.bindings.push({action:'selectView',viewId:cam.viewId,slot:'secondary',input:{kind:'joystick',deviceId:2,definition:'joystick:2:button:6',label:'Very long controller name - Button 7'}});
  s.working.settings.ui.nameDisplay='icons';FSCDemo.setOwnership('Observing');return {camera:cam.id,first:first.id,second:p.categories[1].id};});
 const browserPanel=page.locator('.mc-browser'),tree=page.locator('#mc-camera-tree');
 assert.equal(await page.locator('.mc-browser-title #mc-toggle-categories').count(),1,'Expansion action shares library title toolbar');
 for(const [width,height] of [[1280,900],[900,550]]){await page.setViewportSize({width,height});await page.waitForTimeout(80);
  const dims=await tree.evaluate(el=>({client:el.clientWidth,scroll:el.scrollWidth}));assert.ok(dims.scroll<=dims.client+1,'No horizontal library overflow');
  const rows=await page.locator('.mc-camera-row').evaluateAll(rows=>rows.map(row=>{const r=row.getBoundingClientRect(),name=row.querySelector('.mc-entity-name').getBoundingClientRect(),key=row.querySelector('.mc-key'),star=row.querySelector('.mc-row-star').getBoundingClientRect(),label=row.querySelector('.mc-entity-label');return {height:r.height,label:getComputedStyle(label).display,overlap:key?name.right>key.getBoundingClientRect().left+2:false,star:star.right<=r.right+1};}));
  assert.ok(await page.locator('.mc-camera-rows').evaluateAll(groups=>groups.every(g=>g.getBoundingClientRect().left>g.parentNode.querySelector('.mc-category-line').getBoundingClientRect().left+7)),'Child cameras inset beneath category bands');
  assert.ok(rows.every(r=>r.height===32&&r.label!=='none'&&!r.overlap&&r.star),'Compact single-line labeled camera rows reserve binding and star space');
  assert.ok(await page.locator('.mc-category-line').evaluateAll(rows=>rows.every(row=>{const name=row.querySelector('.mc-entity-name').getBoundingClientRect(),count=row.querySelector('.mc-count').getBoundingClientRect(),menu=row.querySelector('.mc-category-menu').getBoundingClientRect();return name.right<=count.left+1&&count.right<=menu.left+1;})),'Categories reserve independent count and menu space');
  await page.screenshot({path:'build/review/library-layout-main-'+width+'.png'});await browserPanel.screenshot({path:'build/review/library-layout-sidebar-'+width+'.png'});
 }
 await page.setViewportSize({width:1280,height:900});
 const camera=page.locator('[data-focus-key="cam:'+ids.camera+'"]');await camera.focus();await page.keyboard.press('Shift+F10');await page.getByRole('button',{name:'Assign key binding',exact:true}).waitFor();await page.keyboard.press('Escape');
 await page.locator('#mc-search').fill('Taxiway');assert.equal(await page.locator('.mc-category-line').count(),1);assert.equal(await page.locator('.mc-camera-row').count(),75);await page.locator('#mc-search').fill('');
 await page.locator('#mc-toggle-categories').click();assert.equal(await page.locator('#mc-toggle-categories').innerText(),'Collapse all');await page.locator('#mc-toggle-categories').click();assert.equal(await page.locator('.mc-camera-row').count(),0);await page.locator('[data-focus-key="cat:'+ids.first+'"]').click();
 const grip=page.locator('.mc-camera-row').filter({has:page.locator('[data-focus-key="cam:'+ids.camera+'"]')}).locator('.mc-reorder-grip');const from=await grip.boundingBox(),to=await page.locator('[data-focus-key="cat:'+ids.second+'"]').boundingBox();await page.mouse.move(from.x+from.width/2,from.y+from.height/2);await page.mouse.down();await page.mouse.move(to.x+to.width/2,to.y+to.height/2,{steps:8});await page.mouse.up();
 await page.waitForFunction(ids=>FSCApp.state.working.profile.cameras.find(c=>c.id===ids.camera).categoryId===ids.second,ids);
 assert.deepEqual(errors,[]);console.log('PASS compact main library: title controls, category alignment, camera shortcut/star reservations, labels in icons mode, search/collapse, keyboard context and grip reorder.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
