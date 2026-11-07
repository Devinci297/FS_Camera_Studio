const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
// Mini dock: the line between the categories and the cameras is a button that shows the categories as icons only and
// back. The dock keeps its height, categories still work, the choice survives a reload, and icons-only name display
// (already compact) hides the divider.
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/')+'?surface=mini';
  const page=await browser.newPage({viewport:{width:1520,height:900}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  const load=async()=>{await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);await page.evaluate(()=>document.getElementById('mc-mini-pin').click());await page.waitForTimeout(300);};
  await page.goto(url);await page.evaluate(()=>{try{localStorage.removeItem('fscs.miniCategoriesCollapsed');}catch(e){}});await page.reload();await load();
  const divider=page.locator('#mc-dock-divider');
  const geometry=()=>page.evaluate(()=>{const dock=document.getElementById('mc-mini-panel'),pane=document.getElementById('mc-dock-categories'),d=document.getElementById('mc-dock-divider').getBoundingClientRect(),p=pane.getBoundingClientRect(),label=pane.querySelector('.mc-entity-label');
   return {dock:dock.getBoundingClientRect().height,pane:p.width,paneRight:p.right,divider:{left:d.left,right:d.right,top:d.top,bottom:d.bottom},paneTop:p.top,paneBottom:p.bottom,labelShown:Boolean(label&&label.getClientRects().length),collapsed:dock.dataset.categoriesCollapsed,favoritesLeft:document.getElementById('mc-dock-favorites').getBoundingClientRect().left};});

  // Expanded: names shown; the divider sits on the pane's right border, full height.
  assert.ok(await divider.isVisible(),'divider is shown');
  assert.equal(await divider.getAttribute('aria-expanded'),'true');
  const open=await geometry();
  assert.equal(open.collapsed,'false');assert.ok(open.labelShown,'names shown when expanded');
  assert.ok(open.divider.left<open.paneRight&&open.divider.right>open.paneRight,'divider straddles the border '+JSON.stringify(open));
  assert.ok(Math.abs(open.divider.top-open.paneTop)<=1&&Math.abs(open.divider.bottom-open.paneBottom)<=1,'divider spans the pane');

  // Collapse: icons only, narrower column, same dock height, cameras move left.
  await divider.click();
  const closed=await geometry();
  assert.equal(closed.collapsed,'true');assert.equal(closed.labelShown,false,'names hidden when collapsed');
  assert.equal(await divider.getAttribute('aria-expanded'),'false');
  assert.match(await divider.getAttribute('aria-label'),/Show category names/);
  assert.ok(closed.pane<open.pane/2,'collapsed column is narrow: '+closed.pane+' vs '+open.pane);
  assert.equal(closed.dock,open.dock,'dock height unchanged');
  assert.ok(closed.favoritesLeft<open.favoritesLeft-100,'cameras get the room');
  assert.ok(closed.divider.left<closed.paneRight&&closed.divider.right>closed.paneRight,'divider follows the border');
  // Icons stay usable: picking a category still shows its cameras, and the Traffic tab still opens.
  const external=page.locator('#mc-dock-categories button[aria-label=External]');
  await external.click();
  assert.equal(await external.getAttribute('aria-pressed'),'true');
  await page.locator('#mc-dock-traffic-tab').click();
  await page.waitForFunction(()=>document.getElementById('mc-dock-traffic'));
  assert.equal((await geometry()).collapsed,'true','stays collapsed across dock re-renders');

  // Remembered across a reload (a new flight or panel reopen).
  await page.reload();await load();
  assert.equal((await geometry()).collapsed,'true','collapsed state is remembered');
  await page.locator('#mc-dock-divider').click();
  const reopened=await geometry();
  assert.equal(reopened.collapsed,'false');assert.ok(reopened.labelShown);
  assert.equal(await page.evaluate(()=>localStorage.getItem('fscs.miniCategoriesCollapsed')),'false');

  // Icons-only name display is already compact: no divider.
  await page.evaluate(()=>{document.getElementById('mc-sim').dataset.nameDisplay='icons';window.dispatchEvent(new Event('resize'));});
  await page.waitForFunction(()=>document.getElementById('mc-dock-divider').hidden);
  assert.deepEqual(errors,[]);
  console.log('Mini category divider: toggle, icons-only collapsed column, stable dock height, categories usable, remembered, hidden for icons-only display passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
