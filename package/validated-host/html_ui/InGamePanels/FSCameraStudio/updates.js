/* Official release checks and an explicit handoff to the installed Windows updater. */
(function (global) {
  'use strict';
  const REPO = 'https://github.com/Devinci297/FS_Camera_Studio';
  const API = 'https://api.github.com/repos/Devinci297/FS_Camera_Studio/releases?per_page=100';
  const INTERVAL = 6 * 60 * 60 * 1000, RETRY = 15 * 60 * 1000, STORE = 'fscs.updates.v2';
  function version(value) {
    const m = /^v?(\d+)\.(\d+)\.(\d+)(?:-test\.(\d{8})\.(\d+))?$/.exec(value || '');
    if (!m) return null;
    const parts = [Number(m[1]), Number(m[2]), Number(m[3]), m[4] ? 0 : 1, Number(m[4] || 0), Number(m[5] || 0)];
    return parts.every(n => Number.isSafeInteger(n)) ? parts : null;
  }
  function compare(a, b) {
    const x = version(a), y = version(b); if (!x || !y) return null;
    for (let i = 0; i < x.length; ++i) if (x[i] !== y[i]) return x[i] > y[i] ? 1 : -1;
    return 0;
  }
  function latest(releases, current) {
    if (!Array.isArray(releases) || !version(current)) throw new Error('Invalid update response.');
    let found = null;
    releases.forEach(release => {
      if (!release || release.draft || !version(release.tag_name)) return;
      const tag = release.tag_name, id = tag.replace(/^v/, '');
      if ((release.prerelease || id.includes('-')) && !current.includes('-test.')) return;
      const file = 'FS-Camera-Studio-' + id + '-Setup.exe';
      const url = REPO + '/releases/download/' + tag + '/' + file;
      const asset = (Array.isArray(release.assets) ? release.assets : []).find(a => a && a.name === file && a.browser_download_url === url && Number.isSafeInteger(a.size) && a.size > 0 && a.size <= 256 * 1024 * 1024 && a.state === 'uploaded');
      if (asset && compare(tag, current) > 0 && (!found || compare(tag, found.version) > 0)) found = {version: id, url, notes: REPO + '/releases/tag/' + tag};
    });
    return found;
  }
  const exported = {version, compare, latest};
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  if (!global.document) return;
  const doc=global.document, settings=doc.getElementById('mc-page-settings'), root=doc.getElementById('mc-sim');
  if (!settings || !root) return;
  const current=global.FSCS_BUILD_VERSION || 'unknown', isXP=/[?&]host=xplane\b/.test(global.location.search);
  const el=(tag,text,cls)=>{const e=doc.createElement(tag);if(text)e.textContent=text;if(cls)e.className=cls;return e;};
  const button=(text,id,cls)=>{const e=el('button',text,cls||'mc-secondary cursor-interaction');e.type='button';e.id=id;return e;};
  function read(key,fallback){try{const s=global.localStorage.getItem(key);return s===null?fallback:JSON.parse(s);}catch(_){return fallback;}}
  function write(key,value){try{global.localStorage.setItem(key,JSON.stringify(value));}catch(_){}}
  let candidate=null,busy=false,launching=false,lastAttempt=0,lastSuccess=0,dismissed=read('fscs.updateDismissed',''),xhr=null,miniFallback=false;
  const box=el('section','','mc-update-settings');box.id='mc-update-settings';
  const status=el('p','Checks official releases automatically every six hours while a panel is open.','mc-note');status.id='mc-update-status';status.setAttribute('role','status');
  const checked=el('p','','mc-note');checked.id='mc-update-last-check';
  const check=button('Check for updates','mc-update-check');
  const update=button('Update now','mc-update-install','mc-primary cursor-interaction');update.hidden=true;
  const download=button('Download installer','mc-update-download');download.hidden=true;
  const notes=button('Release notes','mc-update-notes');notes.hidden=true;
  const link=el('input');link.type='text';link.readOnly=true;link.hidden=true;link.setAttribute('aria-label','Update installer link');
  const actions=el('div','','mc-update-actions');[check,update,download,notes].forEach(e=>actions.appendChild(e));
  const auto=el('input','','mc-switch');auto.id='mc-update-auto';auto.type='checkbox';auto.checked=read('fscs.checkUpdates',true)!==false;
  const label=el('label','','mc-switch-row');label.appendChild(el('span','Automatically check and notify me','mc-switch-copy'));label.appendChild(auto);
  const hint=el('p','Update now opens the Windows updater. It verifies the download and waits for MSFS and X-Plane to close before upgrading. Save your views first. Profiles are preserved. If the updater does not open, choose Download installer.','mc-note');
  [el('h4','Version & updates'),el('p','Installed build: '+current,'mc-note'),status,checked,actions,link,hint,label].forEach(e=>box.appendChild(e));settings.appendChild(box);
  const badge=button('v'+current.split('-')[0],'mc-app-version','mc-version cursor-interaction');badge.title='Version '+current+' and updates';badge.setAttribute('aria-label',badge.title);
  const brand=doc.querySelector('.mc-brand');if(brand)brand.appendChild(badge);
  badge.addEventListener('click',()=>{doc.getElementById('mc-nav-settings').click();global.setTimeout(()=>{const nav=doc.getElementById('mc-settings-link-updates');if(nav)nav.click();box.scrollIntoView({block:'nearest'});},0);});
  const notice=el('div','','mc-update-notice');notice.id='mc-update-notice';notice.hidden=true;notice.setAttribute('role','status');notice.setAttribute('aria-live','polite');
  const message=el('span'),noticeUpdate=button('Update now','mc-update-notice-install','mc-primary cursor-interaction'),later=button('Later','mc-update-later');
  [message,noticeUpdate,later].forEach(e=>notice.appendChild(e));
  const banners=doc.getElementById('mc-banners');if(banners)banners.insertAdjacentElement('afterend',notice);
  const mini=button('Update','mc-mini-update','mc-update-mini cursor-interaction');mini.hidden=true;mini.setAttribute('aria-label','Update available: open updater');
  const dock=doc.getElementById('mc-mini-panel');if(dock)dock.appendChild(mini);
  function render(){
    [update,download,notes].forEach(e=>e.hidden=!candidate);
    update.disabled=noticeUpdate.disabled=mini.disabled=launching;
    notice.hidden=!candidate || dismissed===candidate.version;
    mini.hidden=!candidate;
    badge.textContent='v'+current.split('-')[0]+(candidate?' - Update':'');badge.classList.toggle('mc-has-update',!!candidate);
    if(candidate){message.textContent='Camera Studio '+candidate.version+' is available.';link.value=candidate.url;mini.textContent=miniFallback?'Download installer':'Update available - Update now';mini.title=miniFallback?'Download the official installer':'Update to '+candidate.version+' after closing the simulators';}
    checked.textContent=lastSuccess?'Last checked: '+new Date(lastSuccess).toLocaleString():'';
    if(global.FSCApp && global.FSCApp.refreshMiniLayout)global.FSCApp.refreshMiniLayout();
    if(global.fscsUpdateMiniRect)global.fscsUpdateMiniRect();
  }
  function restore(){
    const stored=read(STORE,null);
    if(stored && stored.current===current && Number.isFinite(stored.at) && stored.at<=Date.now() && Date.now()-stored.at<INTERVAL){
      try{candidate=latest(stored.releases,current);lastSuccess=stored.at;render();if(candidate)status.textContent='Update available: '+candidate.version;}catch(_){}
    }
  }
  function checkUpdates(manual){
    if(busy)return;busy=true;lastAttempt=Date.now();check.disabled=true;status.textContent='Checking GitHub...';
    xhr=new XMLHttpRequest();xhr.open('GET',API,true);xhr.timeout=15000;xhr.setRequestHeader('Accept','application/vnd.github+json');
    const finish=error=>{busy=false;check.disabled=false;if(error)status.textContent=error+(candidate?' The previously found update is still available.':' Try again later.');};
    xhr.onload=()=>{try{
      if(xhr.status!==200)throw new Error('Update check failed (HTTP '+xhr.status+').');
      if(xhr.responseText.length>2*1024*1024)throw new Error('Update response was too large.');
      const releases=JSON.parse(xhr.responseText);candidate=latest(releases,current);lastSuccess=Date.now();
      // Store only the selected release's public installer metadata, then validate it again when read.
      const selected=candidate?releases.filter(r=>r && typeof r.tag_name==='string' && r.tag_name.replace(/^v/,'')===candidate.version).map(r=>({tag_name:r.tag_name,draft:r.draft,prerelease:r.prerelease,assets:r.assets.filter(a=>a.browser_download_url===candidate.url)})):[];
      write(STORE,{current,at:lastSuccess,releases:selected});
      if(manual && candidate){dismissed='';write('fscs.updateDismissed','');}
      status.textContent=candidate?'Update available: '+candidate.version:'You have the latest available build for this channel.';
      render();finish();
    }catch(e){finish(e.message);}};
    xhr.onerror=()=>finish('Unable to reach GitHub.');xhr.ontimeout=()=>finish('The update check timed out.');xhr.onabort=()=>finish('Update check cancelled.');
    try{xhr.send();}catch(_){finish('Unable to start the update check.');}
  }
  async function open(url){
    if(isXP && global.xplane && typeof global.xplane.fscsOpenUrl==='function'){
      const result=await global.xplane.fscsOpenUrl(url);if(result===false)throw new Error('Windows could not open the link.');return;
    }
    if(typeof global.OpenBrowser==='function'){global.OpenBrowser(url);return;}
    const opened=global.open(url,'_blank','noopener');if(!opened)throw new Error('Open the link in your browser.');
  }
  function feedback(text,tone){status.textContent=text;if(root.dataset.surface==='mini' && global.FSCApp && global.FSCApp.announce)global.FSCApp.announce(text,tone||'ok',6500);}
  async function install(){
    if(!candidate || launching)return;
    launching=true;render();link.hidden=false;
    try{
      if(isXP && global.xplane && typeof global.xplane.fscsUpdate==='function'){
        const result=await global.xplane.fscsUpdate(candidate.version);
        if(!result || result.ok!==true)throw new Error(result&&result.error||'The Windows updater is not installed.');
        feedback('Updater opened. Save your views and close MSFS and X-Plane when ready; the verified update will install afterward.');
      }else if(typeof global.OpenBrowser==='function'){
        global.OpenBrowser('fscamerastudio://update/'+candidate.version);
        feedback('Windows updater requested. Save your views and close both simulators when ready. If no updater opens, choose Download installer.');
      }else throw new Error('Install the current Windows setup to enable click-to-update.');
    }catch(e){miniFallback=true;feedback(e.message+(root.dataset.surface==='mini'?' Click Download for the installer.':' Use Download installer below.'),'error');}
    finally{launching=false;render();}
  }
  [update,noticeUpdate].forEach(e=>e.addEventListener('click',install));
  mini.addEventListener('click',()=>{if(miniFallback)download.click();else install();});
  download.addEventListener('click',async()=>{if(!candidate)return;link.hidden=false;link.select();try{await open(candidate.url);feedback('Download requested. Save your views, close both simulators and run setup.');}catch(_){status.textContent='Copy the installer link below into your browser. Close both simulators before running setup.';}});
  notes.addEventListener('click',()=>{if(candidate)open(candidate.notes).catch(()=>{link.hidden=false;link.value=candidate.notes;link.select();});});
  later.addEventListener('click',()=>{if(candidate){dismissed=candidate.version;write('fscs.updateDismissed',dismissed);render();}});
  check.addEventListener('click',()=>checkUpdates(true));
  auto.addEventListener('change',()=>{write('fscs.checkUpdates',auto.checked);if(auto.checked)maybeCheck();});
  function maybeCheck(){
    const app=global.FSCApp,now=Date.now();
    if(!auto.checked || busy || doc.hidden || !app || !app.state.snapshot || app.bridge.demo)return;
    if(lastSuccess && now-lastSuccess<INTERVAL)return;
    if(lastAttempt && now-lastAttempt<RETRY)return;
    restore();if(lastSuccess && now-lastSuccess<INTERVAL)return;
    // Avoid duplicate requests when the main and mini share storage. Expired leases recover a closed panel.
    const lease=read('fscs.updateLease',0);if(Number.isFinite(lease) && lease>now && lease<now+60000)return;
    write('fscs.updateLease',now+20000);checkUpdates(false);
  }
  doc.addEventListener('visibilitychange',maybeCheck);global.addEventListener('online',()=>{lastAttempt=0;maybeCheck();});
  global.addEventListener('storage',e=>{if(e.key===STORE)restore();if(e.key==='fscs.checkUpdates')auto.checked=read('fscs.checkUpdates',true)!==false;if(e.key==='fscs.updateDismissed'){dismissed=read('fscs.updateDismissed','');render();}});
  new MutationObserver(maybeCheck).observe(root,{attributes:true,attributeFilter:['data-mode','data-enabled']});
  global.setTimeout(maybeCheck,5000);global.setInterval(maybeCheck,30000);
  restore();global.FSCSUpdates=Object.assign(exported,{check:()=>checkUpdates(true),maybeCheck});
})(typeof window!=='undefined'?window:globalThis);
