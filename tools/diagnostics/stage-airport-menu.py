"""Expose airport generation in the library + menu on the known-loading UI."""
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
baseline = root / 'build/package/Packages/fs-camera-studio'
candidate = root / 'build/diagnostic/airport-menu/fs-camera-studio'
review = root / 'build/review/airport-menu-20261011'
prior = json.loads((root / 'build/review/movement-bindings-20261011/installed.json').read_text(encoding='utf-8'))

def inventory(folder):
    return {p.relative_to(folder).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

assert inventory(baseline)==prior['files'], 'Installed baseline changed.'
assert not candidate.exists(), 'Inspect existing candidate before restaging.'
source=(root/'ui/app.js').read_text(encoding='utf-8')
text=(baseline/'html_ui/InGamePanels/FSCameraStudio/app.js').read_text(encoding='utf-8')
def replace(old,new):
    global text
    assert text.count(old)==1,(old[:100],text.count(old))
    text=text.replace(old,new,1)

# No discovery, generation or rendering starts automatically. Airport operations are opened on demand.
replace("    miniCategory: null,", '    airports: {nearby:[],facilities:[],loaded:"",radiusNm:25}, airportPreviews: {},\n    miniCategory: null,')
replace('    S.profiles = p.profiles || [];','    S.profiles = p.profiles || [];\n    S.airports = p.airports || S.airports;')
replace('    Snapshot: applySnapshot,', '''    Snapshot: applySnapshot,
    AirportsChanged(p) { S.airports=p; if($('mc-airport-nearby'))renderAirportNearby(); },
    AirportPreview(p) { S.airportPreviews[p.icao]=p; if($('mc-airport-results'))renderAirportPreview(); },''')
replace("    if (dialogOpen()) closeDialog(true);\n    dialogOpener", "    if (dialogOpen()) closeDialog(true);\n    $('mc-dialog-shade').classList.remove('mc-airport-shade');\n    dialogOpener")
start=source.index('  function airportDialog() {')
end=source.index('  function renderProfiles()',start)
replace('  function renderProfiles() {',source[start:end]+'  function renderProfiles() {')
old="[['New camera', 'camera', addCamera], ['New category', 'folder-plus', addCategory]]"
new="[['New camera', 'camera', addCamera], ['New category', 'folder-plus', addCategory], ['Create airport cameras', 'map-pin', airportDialog]]"
assert new in source
replace(old,new)
replace('window.FSCApp = { state: S, bridge };','window.FSCApp = { state: S, bridge, parseAirportXml };')
shutil.copytree(baseline,candidate)
(candidate/'html_ui/InGamePanels/FSCameraStudio/app.js').write_text(text,encoding='utf-8')
css=(root/'ui/app.css').read_text(encoding='utf-8')
rules='\n'.join(line for line in css.splitlines() if any(selector in line for selector in ['.mc-airport-results','.mc-airport-options','.mc-airport-shade','#mc-airport-codes','#mc-airport-nearby']))
assert len(rules.splitlines())==12, len(rules.splitlines())
with (candidate/'html_ui/InGamePanels/FSCameraStudio/app.css').open('a',encoding='utf-8') as f:
    f.write('\n/* On-demand airport creation dialog. */\n'+rules+'\n')
layout=json.loads((candidate/'layout.json').read_text(encoding='utf-8'))
for entry in layout['content']:
    p=candidate/entry['path'];entry['size']=p.stat().st_size;entry['date']=p.stat().st_mtime_ns//100+116444736000000000
(candidate/'layout.json').write_text(json.dumps(layout,indent=2),encoding='utf-8')
files=inventory(candidate)
changed=sorted(name for name in files if files[name]!=prior['files'][name])
assert changed==['html_ui/InGamePanels/FSCameraStudio/app.css','html_ui/InGamePanels/FSCameraStudio/app.js','layout.json'],changed
review.mkdir(parents=True,exist_ok=True)
(review/'staged.json').write_text(json.dumps({
    'status':'staged-not-installed','createdUtc':datetime.now(timezone.utc).isoformat(),
    'candidate':str(candidate),'changed':changed,'files':files,'baselineFiles':prior['files'],
    'purpose':'Library + menu: Create airport cameras, with on-demand nearby/ICAO/XML preview and selective save.',
    'moduleUnchanged':True,'liveVerified':False
},indent=2),encoding='utf-8')
print(f'Staged {len(files)} files; only changes: {changed}. Module unchanged; installed package untouched.')
