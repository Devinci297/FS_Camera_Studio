/* Public GitHub releases only. No package files are changed by the running panel. */
(function (global) {
  'use strict';
  const REPO = 'https://github.com/Devinci297/FS_Camera_Studio';
  const API = 'https://api.github.com/repos/Devinci297/FS_Camera_Studio/releases?per_page=100';
  function version(value) {
    const m = /^v?(\d+)\.(\d+)\.(\d+)(?:-test\.(\d{8})\.(\d+))?$/.exec(value || '');
    return m ? [Number(m[1]), Number(m[2]), Number(m[3]), m[4] ? 0 : 1, Number(m[4] || 0), Number(m[5] || 0)] : null;
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
      if (release.prerelease && current.indexOf('-test.') < 0) return;
      const tag = release.tag_name, id = tag.replace(/^v/, '');
      const file = 'FS-Camera-Studio-' + id + '-Setup.exe';
      const url = REPO + '/releases/download/' + tag + '/' + file;
      const asset = (Array.isArray(release.assets) ? release.assets : []).find(a => a.name === file && a.browser_download_url === url && a.size > 0 && a.state === 'uploaded');
      if (asset && compare(tag, current) > 0 && (!found || compare(tag, found.version) > 0)) found = {version: id, url, notes: REPO + '/releases/tag/' + tag};
    });
    return found;
  }
  const exported = {version, compare, latest};
  if (typeof module !== 'undefined' && module.exports) module.exports = exported;
  if (!global.document) return;
  const doc = global.document, settings = doc.getElementById('mc-page-settings'), root = doc.getElementById('mc-sim');
  if (!settings || !root) return;
  const current = global.FSCS_BUILD_VERSION || 'unknown';
  const el = (tag, text, cls) => { const e = doc.createElement(tag); if (text) e.textContent = text; if (cls) e.className = cls; return e; };
  const box = el('section', '', 'mc-update-settings');box.id = 'mc-update-settings';
  const heading = el('h4', 'Version & updates'), build = el('p', 'Installed build: ' + current, 'mc-note');
  const status = el('p', 'Checks official GitHub releases, including tester builds.', 'mc-note');status.id = 'mc-update-status';status.setAttribute('role','status');
  const check = el('button', 'Check for updates', 'mc-secondary cursor-interaction');check.type = 'button';check.id = 'mc-update-check';
  const download = el('button', 'Download update', 'mc-primary cursor-interaction');download.type='button';download.hidden=true;download.id='mc-update-download';
  const link = el('input');link.type='text';link.readOnly=true;link.hidden=true;link.setAttribute('aria-label','Update installer link');
  const hint = el('p', 'Download the installer, save your views, then close MSFS and run setup. Your saved profiles are preserved.', 'mc-note');
  const choices = el('div','','mc-update-actions');choices.appendChild(check);choices.appendChild(download);
  const label = el('label','','mc-switch-row');const labelText=el('span','Check when the editor opens','mc-switch-copy');const auto=el('input','','mc-switch');auto.type='checkbox';
  let preference=true;try {preference=global.localStorage.getItem('fscs.checkUpdates')!=='false';}catch (_){}auto.checked=preference;
  auto.addEventListener('change',()=>{try {global.localStorage.setItem('fscs.checkUpdates',String(auto.checked));}catch(_){}maybeCheck();});label.appendChild(labelText);label.appendChild(auto);
  [heading,build,status,choices,link,hint,label].forEach(e=>box.appendChild(e));settings.appendChild(box);
  const badge=el('button','v'+current.split('-')[0],'mc-version cursor-interaction');badge.type='button';badge.id='mc-app-version';badge.title='Build '+current+' - check for updates';badge.setAttribute('aria-label','Version '+current+' and updates');
  const brand=doc.querySelector('.mc-brand');if(brand)brand.appendChild(badge);
  badge.addEventListener('click',()=>{doc.getElementById('mc-nav-settings').click();box.scrollIntoView({block:'nearest'});});
  let candidate=null, busy=false, attempted=false;
  function checkUpdates() {
    if(busy)return;busy=true;attempted=true;check.disabled=true;download.hidden=true;link.hidden=true;candidate=null;
    status.textContent='Checking GitHub…';
    const xhr=new XMLHttpRequest();xhr.open('GET',API,true);xhr.timeout=15000;xhr.setRequestHeader('Accept','application/vnd.github+json');
    function finish(error){busy=false;check.disabled=false;if(error)status.textContent=error+' Try again, or visit the GitHub releases page.';}
    xhr.onload=()=>{try {
      if(xhr.status!==200)throw new Error('Update check failed (HTTP '+xhr.status+').');
      if(xhr.responseText.length>2*1024*1024)throw new Error('Update response was too large.');
      candidate=latest(JSON.parse(xhr.responseText),current);
      if(candidate){status.textContent='Update available: '+candidate.version;download.hidden=false;link.value=candidate.url;badge.textContent='v'+current.split('-')[0]+' · Update';}
      else status.textContent='You have the latest available build for this channel.';
      finish();
    }catch(error){finish(error.message);}};
    xhr.onerror=()=>finish('Unable to reach GitHub.');xhr.ontimeout=()=>finish('The update check timed out.');
    try{xhr.send();}catch(error){finish('Unable to start the update check.');}
  }
  check.addEventListener('click',checkUpdates);
  download.addEventListener('click',()=>{
    if(!candidate)return;link.hidden=false;link.select();
    try {if(typeof global.OpenBrowser==='function')global.OpenBrowser(candidate.url);else global.open(candidate.url,'_blank','noopener');
      status.textContent='Download requested. Close MSFS before running setup. If no browser opened, copy the installer link below.';
    }catch(_){status.textContent='Copy the installer link below into your browser. Close MSFS before running setup.';}
  });
  link.addEventListener('focus',()=>link.select());
  function maybeCheck(){
    const app=global.FSCApp;
    if(!attempted && auto.checked && app && app.state.snapshot && !app.bridge.demo && root.dataset.mode==='edit' && !doc.hidden)checkUpdates();
  }
  // Never poll or run HTTP from the WASM frame loop or flight-loading callback.
  new MutationObserver(maybeCheck).observe(root,{attributes:true,attributeFilter:['data-mode']});
  global.setTimeout(maybeCheck,5000);
  global.FSCSUpdates=exported;
})(typeof window!=='undefined'?window:globalThis);
