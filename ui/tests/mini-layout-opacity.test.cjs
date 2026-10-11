const {chromium}=require(process.env.FSCS_PLAYWRIGHT||'playwright');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',headless:true});
 try {
  const page=await browser.newPage({viewport:{width:1700,height:900},bypassCSP:true}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  const url='file:///'+path.resolve(process.argv[2]||'ui/index.html').replaceAll('\\','/');
  await page.goto(url.replace(/main\.html$/,'mini.html')+'?surface=mini');
  await page.waitForFunction(()=>window.FSCApp&&FSCApp.state.snapshot);
  await page.evaluate(()=>{
   const s=FSCApp.state,p=s.working.profile;
   p.categories=p.categories.slice(0,4);
   for(let i=0;i<5;i++){const g=JSON.parse(JSON.stringify(p.categories[0]));g.id=crypto.randomUUID();g.name=['Runways','Gates','Apron','Taxiway intersections','Tower'][i];g.airportIcao='GOBD';p.categories.push(g);}
   s.working.settings.ui.miniHover=false;FSCDemo.setOwnership('Observing');document.getElementById('mc-mini-pin').click();
  });
  await page.waitForTimeout(350);
  for(const width of [750,1200,1700])for(const mode of ['icons','labels','both']){
   await page.setViewportSize({width,height:900});
   await page.evaluate(mode=>{document.querySelector('#mc-setting-name-display [data-value="'+mode+'"]').click();FSCDemo.setOwnership('Observing');},mode);
   await page.waitForTimeout(250);
   const layout=await page.locator('#mc-dock-categories').evaluate(p=>{
    const r=p.getBoundingClientRect(),buttons=[...p.querySelectorAll('button')];
    return {overflow:p.scrollHeight>p.clientHeight+1,clipped:buttons.filter(b=>{const q=b.getBoundingClientRect();return q.left<r.left-1||q.right>r.right+1||q.top<r.top-1||q.bottom>r.bottom+1}).length,
     smallest:Math.min(...buttons.map(b=>b.getBoundingClientRect().height)),icon:getComputedStyle(p.querySelector('svg')).width,
     unused:r.bottom-Math.max(...buttons.map(b=>b.getBoundingClientRect().bottom))};
   });
   assert.equal(layout.overflow,false,width+'/'+mode+' categories overflow');assert.equal(layout.clipped,0);
   assert.ok(layout.smallest>=42,'Category hit targets should fill available height');
   if(mode!=='labels')assert.equal(layout.icon,'24px');
   assert.ok(layout.unused<4,'Category rows leave unused height');
  }
  await page.locator('#mc-mini-panel').screenshot({path:'build/review/mini-categories-expanded.png'});
  // Emulate stock ingame-ui painting a gradient and content pseudo-element behind our dock.
  await page.evaluate(()=>{
   const root=document.getElementById('mc-sim');let host=document.getElementById('FSCSMini');
   if(!host){host=document.createElement('ingame-ui');host.id='FSCSMini';root.before(host);host.appendChild(root);}
   const wrap=document.createElement('div'),content=document.createElement('div');
   wrap.className='ingameUiWrapper';content.className='ingameUiContent';root.before(wrap);wrap.appendChild(content);content.appendChild(root);
   const style=document.createElement('style');style.textContent='body.surface-mini, #mc-sim, .mc-stage{background-image:linear-gradient(red,red)} .ingameUiContent::before{content:"";position:absolute;inset:0;background:linear-gradient(red,red);pointer-events:none}';document.head.appendChild(style);
  });
  await page.addStyleTag({path:path.resolve('package/panel/panel-host.css')});
  for(const value of [15,65,100]){
   await page.evaluate(value=>{const slider=document.getElementById('mc-setting-mini-opacity');slider.value=String(value);slider.dispatchEvent(new Event('input',{bubbles:true}));},value);
   const layers=await page.evaluate(()=>{
    const dock=document.getElementById('mc-mini-panel');const all=[];let n=dock.parentElement;
    while(n){const s=getComputedStyle(n);all.push({id:n.id||n.className,image:s.backgroundImage,color:s.backgroundColor});n=n.parentElement;}
    const s=getComputedStyle(document.querySelector('.ingameUiContent'),'::before');all.push({id:'content::before',image:s.backgroundImage,color:s.backgroundColor});
    return {background:dock.style.background,opacity:getComputedStyle(dock.querySelector('button')).opacity,all};
   });
   assert.ok(layers.background.includes(value===100?'rgb(14, 25, 35)':'rgba(14, 25, 35, '+value/100+')'),JSON.stringify(layers));
   for(const layer of layers.all){assert.equal(layer.image,'none',layer.id);assert.equal(layer.color,'rgba(0, 0, 0, 0)',layer.id);}
   assert.equal(layers.opacity,'1','Controls stay opaque');
  }
  assert.deepEqual(errors,[]);console.log('Larger mini categories, visible category bounds, and single alpha-controlled background passed.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
