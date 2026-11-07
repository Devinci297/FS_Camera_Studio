const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Walkaround, orbit, fly-by and cinematic each get their own icon. Icons stored automatically before that (view-orbit,
// or view-wheel on the generated Walkaround category) follow the name, while an icon the user picked stays.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=mini';
  const page=await browser.newPage({viewport:{width:1600,height:900}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(url);await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.evaluate(()=>document.getElementById('mc-mini-pin').click());
  const iconOf=name=>page.locator('#mc-dock-categories button[aria-label="'+name+'"] svg').first().getAttribute('data-icon');
  const shown=async()=>({walk:await iconOf('Walkaround'),orbit:await iconOf('Orbit'),flyby:await iconOf('Fly-by')});
  assert.deepEqual(await shown(),{walk:'view-walkaround',orbit:'view-orbit',flyby:'view-flyby'},'each motion category has its own icon');
  assert.equal(await page.locator('#mc-mouse-look-cinematic svg').getAttribute('data-icon'),'view-cinematic');
  for(const n of ['view-walkaround','view-flyby','view-cinematic','view-spotting'])
   assert.ok(await page.evaluate(n=>FSCIcons.pickerNames.indexOf(n)>=0&&FSCIcons.icon(n).children.length>0,n),n+' can be picked');

  // Older profiles stored the shared icon automatically: the name's own icon is shown instead.
  const setIcons=icons=>page.evaluate(icons=>{const p=FSCApp.state.working.profile;p.categories.forEach(g=>{if(icons[g.name]){Object.assign(g,icons[g.name]);}});FSCDemo.setOwnership('Observing');},icons);
  await setIcons({'Walkaround':{icon:'view-wheel'},'Fly-by':{icon:'view-orbit'},'Orbit':{icon:'view-orbit'}});
  await page.waitForTimeout(200);
  assert.deepEqual(await shown(),{walk:'view-walkaround',orbit:'view-orbit',flyby:'view-flyby'},'legacy automatic icons follow the name');
  // An icon the user chose is kept.
  await setIcons({'Walkaround':{icon:'view-wheel',iconCustomized:true},'Fly-by':{icon:'view-orbit',iconCustomized:true}});
  await page.waitForTimeout(200);
  assert.deepEqual(await shown(),{walk:'view-wheel',orbit:'view-orbit',flyby:'view-orbit'},'chosen icons stay');
  assert.deepEqual(errors,[]);
  console.log('Category icons: distinct walkaround / orbit / fly-by / cinematic icons, pickable, legacy automatic icons upgraded, chosen icons kept passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
