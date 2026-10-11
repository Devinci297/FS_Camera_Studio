"""Stage a narrow airport tracking/zoom candidate from the published host; never install."""
import hashlib,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
source=ROOT/'build/release/0.1.1-test.20261011.1/fs-camera-studio'
out=ROOT/'build/review/airport-tracking-fix/fs-camera-studio'
shutil.copytree(source,out,dirs_exist_ok=True)
u=out/'html_ui/InGamePanels/FSCameraStudio'
app=(u/'app.js').read_text(encoding='utf-8')
new=(ROOT/'ui/app.js').read_text(encoding='utf-8')
a=new.index('  function aircraftTrackingOptions(');b=new.index('  async function saveAdjustedViews(',a)
x=app.index('  function aircraftTrackingOptions(');y=app.index('  async function saveAdjustedViews(',x)
app=app[:x]+new[a:b]+app[y:]
app=app.replace('  function openDialog(title, onClose) {','  function openDialog(title, onClose) {\n    $(\'mc-dialog-shade\').classList.remove(\'mc-tracking-shade\');\n    const trackingPanel=$(\'mc-dialog-shade\').querySelector(\'.mc-dialog\');trackingPanel.style.left=\'\';trackingPanel.style.top=\'\';')
# Transplant settings and status rendering only; keep the validated host lifecycle.
for begin,end in [('  function qualificationText() {','  /** Replace'),('  function renderBanners() {','  function ')]:
 a=new.index(begin);b=new.index(end,a+len(begin));x=app.index(begin);y=app.index(end,x+len(begin));app=app[:x]+new[a:b]+app[y:]
app=app.replace("    $('mc-qual-state').textContent = qualificationText();","    $('mc-setting-unqualified').closest('label').hidden=true;\n    $('mc-qual-state').textContent = qualificationText();\n    $('mc-qual-state').parentNode.querySelector('span').textContent='Camera directions';\n    $('mc-qual-run').hidden=true;")
# Same stand-name/suffix handling for optional XML import.
a=new.index("    a.querySelectorAll('TaxiwayParking')");b=new.index("    a.querySelectorAll(",a+10)
x=app.index("    a.querySelectorAll('TaxiwayParking')");y=app.index("    a.querySelectorAll(",x+10)
app=app[:x]+new[a:b]+app[y:]
(u/'version.js').write_text("window.FSCS_BUILD_VERSION = '0.1.1-test.20261011.2';\n",encoding='utf-8')
(u/'app.js').write_text(app,encoding='utf-8')
css=(ROOT/'ui/app.css').read_text(encoding='utf-8');css=css[css.index('/* Airport tracking:'):]
with (u/'app.css').open('a',encoding='utf-8') as f:f.write('\n'+css)
host=(u/'msfs-panel.js').read_text(encoding='utf-8')
host=host.replace('const bounds = dialog.getBoundingClientRect();','const bounds = (dialog.classList.contains("mc-tracking-shade") ? dialog.querySelector(".mc-dialog") : dialog).getBoundingClientRect();')
host=host.replace('left = bounds.left; right = bounds.right; top = bounds.top; bottom = bounds.bottom;','left = Math.min(left,bounds.left); right = Math.max(right,bounds.right); top = Math.min(top,bounds.top); bottom = Math.max(bottom,bounds.bottom);')
(u/'msfs-panel.js').write_text(host,encoding='utf-8')
shutil.copy2(ROOT/'build/wasm/airport-tracking-fix/FSCameraStudio.wasm',out/'modules/FSCameraStudio.wasm')
entries=[]
for p in sorted(out.rglob('*')):
 if p.is_file() and p.name not in ['manifest.json','layout.json']:
  entries.append(dict(path=p.relative_to(out).as_posix(),size=p.stat().st_size,date=p.stat().st_mtime_ns//100+116444736000000000))
(out/'layout.json').write_text(json.dumps({'content':entries},indent=2),encoding='utf-8')
(out.parent/'staged.json').write_text(json.dumps(dict(status='staged-not-installed',liveVerified=False,files={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file()}),indent=2),encoding='utf-8')
print(out)
