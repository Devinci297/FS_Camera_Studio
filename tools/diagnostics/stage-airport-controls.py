import hashlib,json,shutil
from pathlib import Path
from datetime import datetime,timezone
from flight_controls_ui import patch
root=Path(__file__).resolve().parents[2]
baseline=root/'build/package/Packages/fs-camera-studio'
candidate=root/'build/diagnostic/airport-controls/fs-camera-studio'
review=root/'build/review/airport-controls-20261011'
prior=json.loads((root/'build/review/airport-menu-20261011/installed.json').read_text(encoding='utf-8'))
def inventory(folder):
 return {p.relative_to(folder).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.rglob('*')) if p.is_file()}
assert inventory(baseline)==prior['files'],'Installed baseline changed'
if candidate.exists():
 assert inventory(candidate)==json.loads((review/'staged.json').read_text(encoding='utf-8'))['files'],'Candidate changed since staging'
source=(root/'ui/app.js').read_text(encoding='utf-8')
text=patch((baseline/'html_ui/InGamePanels/FSCameraStudio/app.js').read_text(encoding='utf-8'))
start=source.index('  function airportDialog() {');end=source.index('  function renderProfiles()',start)
a=text.index('  function airportDialog() {');b=text.index('  function renderProfiles()',a)
text=text[:a]+source[start:end]+text[b:]
# Include the airport parent category while retaining the baseline host and input lifecycle.
start=source.index('  function renderDock() {');end=source.index('  function dockViewButton(',start)
a=text.index('  function renderDock() {');b=text.index('  function dockViewButton(',a)
text=text[:a]+source[start:end]+text[b:]
shutil.copytree(baseline,candidate,dirs_exist_ok=True)
ui=candidate/'html_ui/InGamePanels/FSCameraStudio'
(ui/'app.js').write_text(text,encoding='utf-8')
css=(root/'ui/app.css').read_text(encoding='utf-8')
with (ui/'app.css').open('a',encoding='utf-8') as f:f.write('\n'+css[css.index('/* Airport creation: explicit controls'):])
bridge=(ui/'bridge.js').read_text(encoding='utf-8')
old='    return { schemaVersion: 2, movementDefaultsVersion: 1, bindings: list };'
assert bridge.count(old)==1
bridge=bridge.replace(old,"    list.push({action:'saveViews',input:{kind:'key',definition:'VK_LCONTROL+S',label:'Ctrl + S'}});\n    return { schemaVersion: 2, movementDefaultsVersion: 1, saveViewsDefaultsVersion:1, bindings: list };")
(ui/'bridge.js').write_text(bridge,encoding='utf-8')
shutil.copy2(root/'build/wasm/airport-controls-20261011/FSCameraStudio.wasm',candidate/'modules/FSCameraStudio.wasm')
layout=json.loads((candidate/'layout.json').read_text(encoding='utf-8'))
for e in layout['content']:
 p=candidate/e['path'];e['size']=p.stat().st_size;e['date']=p.stat().st_mtime_ns//100+116444736000000000
(candidate/'layout.json').write_text(json.dumps(layout,indent=2),encoding='utf-8')
files=inventory(candidate);changed=sorted(n for n in files if files[n]!=prior['files'][n])
assert changed==['html_ui/InGamePanels/FSCameraStudio/app.css','html_ui/InGamePanels/FSCameraStudio/app.js','html_ui/InGamePanels/FSCameraStudio/bridge.js','layout.json','modules/FSCameraStudio.wasm'],changed
review.mkdir(parents=True,exist_ok=True)
(review/'staged.json').write_text(json.dumps(dict(status='staged-not-installed',createdUtc=datetime.now(timezone.utc).isoformat(),candidate=str(candidate),changed=changed,files=files,baselineFiles=prior['files'],purpose='Airport movement/tracking/auto switch, view-relative movement, per-camera head tracking and temporary look, Save and expanded binding slots, compact scrolling mini panel.',liveVerified=False),indent=2),encoding='utf-8')
print('Staged',len(files),'files; changed',changed)
