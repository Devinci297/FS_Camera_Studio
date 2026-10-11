"""Layer reviewed input/UI fixes on the simulator-tested .4 host without replacing its mouse plumbing."""
import hashlib, json, re, shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/review/panel-polish-a320'
OUT = ROOT / 'build/review/input-release-fix/fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

assert inventory(BASE/'fs-camera-studio') == json.loads((BASE/'staged.json').read_text())['files']
shutil.copytree(BASE/'fs-camera-studio', OUT, dirs_exist_ok=True)
ui = OUT/'html_ui/InGamePanels/FSCameraStudio'
source = (ROOT/'ui/app.js').read_text(encoding='utf-8')
app = (ui/'app.js').read_text(encoding='utf-8')

def function(text, name):
    start = re.search(r'^  (?:async )?function '+name+r'\(', text, re.M).start()
    end = text.index('\n  }', start)+5
    return start, end, text[start:end]

for name in ['renderDock','layoutMiniCategories','aircraftTrackingOptions','wireContext','actionDialog',
             'cameraActions','categoryActions','saveAdjustedViews','openDialog']:
    a,b,_ = function(app,name)
    app = app[:a]+function(source,name)[2]+app[b:]
# The verified host owns pointer blocking; it deliberately has no development pointer reporter.
app=app.replace('      reportPanelPointer(true, true, true); action();','      action();',1)
for name in ['followAutomaticDockCamera','revealAutomaticDockCamera','compactEntityMenu',
             'readCameraSettingsClipboard','copyCameraSettings','cameraSettingsPoseReason','pasteCameraSettings']:
    app=app.replace('  const HAS_POINTER',function(source,name)[2]+'\n\n  const HAS_POINTER',1)
start=source.index('  const CAMERA_SETTINGS_KEY')
end=source.index('  function readCameraSettingsClipboard',start)
app=app.replace('  const HAS_POINTER',source[start:end]+'  const HAS_POINTER',1)
old="AirportsChanged(p) { S.airports=p; renderCameraTools(); if($('mc-airport-nearby'))renderAirportNearby(); },"
new=re.search(r'^    AirportsChanged\(p\).*$',source,re.M).group(0).strip().replace('renderAirportSummary(); ', '')
assert old in app
app=app.replace(old,new)
app=app.replace('    CommitCompleted(p) {',"    CamerasSaved(p) { if(window.FSCSSaveFeedback)window.FSCSSaveFeedback.play(p.requestId); },\n    CommitCompleted(p) {",1)
(ui/'app.js').write_text(app,encoding='utf-8')
css=(ROOT/'ui/app.css').read_text(encoding='utf-8')
with (ui/'app.css').open('a',encoding='utf-8') as f:
    f.write('\n'+css[css.index('/* Camera/category actions remain small popovers'):])
bridge=(ui/'bridge.js').read_text(encoding='utf-8')
bridge=bridge.replace("['moveDown','Q'],['moveUp','E']","['moveUp','Q'],['moveDown','E']").replace('movementDefaultsVersion: 1','movementDefaultsVersion: 2')
(ui/'bridge.js').write_text(bridge,encoding='utf-8')
for name in ['version.js','save-feedback.js']:shutil.copy2(ROOT/'ui'/name,ui/name)
shutil.copytree(ROOT/'ui/audio',ui/'audio',dirs_exist_ok=True)
for name in ['main.html','mini.html']:
    text=(ui/name).read_text(encoding='utf-8')
    text=text.replace("img-src 'self' data:;", "img-src 'self' data:; media-src 'self';")
    text=text.replace('<script src="app.js"></script>','<script src="save-feedback.js"></script>\n<script src="app.js"></script>')
    text=text.replace('Q lower · E raise','Q raise · E lower')
    (ui/name).write_text(text,encoding='utf-8')
module=ROOT/'build/wasm/input-release-fix/FSCameraStudio.wasm'
if module.exists():shutil.copy2(module,OUT/'modules/FSCameraStudio.wasm')
entries=[dict(path=p.relative_to(OUT).as_posix(),size=p.stat().st_size,date=p.stat().st_mtime_ns//100+116444736000000000)
         for p in sorted(OUT.rglob('*')) if p.is_file() and p.name not in ['manifest.json','layout.json']]
(OUT/'layout.json').write_text(json.dumps({'content':entries},indent=2))
(OUT.parent/'staged.json').write_text(json.dumps(dict(status='staged-not-installed',liveVerified=False,
    baseline='0.1.1-test.20261011.4',moduleBuilt=module.exists(),files=inventory(OUT)),indent=2))
print(OUT)
