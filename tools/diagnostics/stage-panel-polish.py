"""Stage scoped UI/profile fixes over the verified .3 simulator host."""
import hashlib, json, re, shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/review/global-airport-auto'
OUT = ROOT / 'build/review/panel-polish-a320/fs-camera-studio'
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
for name in ['decorateName','render','renderDock','renderBindings','applySettings','attachScrollbars',
             'importProfile','showImportReport','exportProfile']:
    a,b,_ = function(app,name)
    app = app[:a]+function(source,name)[2]+app[b:]
for name in ['controlHelp','installTooltips','layoutMiniCategories','updateBindingToggle','exportSelectedProfile']:
    app=app.replace('  const HAS_POINTER',function(source,name)[2]+'\n\n  const HAS_POINTER',1)
assert "  const HAS_POINTER" in app
old="const expand = !(S.bindingExpanded || []).every(Boolean);\n      S.bindingExpanded = [expand, expand, expand, expand];"
assert old in app
app=app.replace(old,"const groups=$$('.mc-binding-section',$('mc-bindings-list'));\n      const expand = !groups.length || !groups.every(g=>g.open);\n      S.bindingExpanded = Array(bindingSections().length+1).fill(expand);")
app=app.replace("setLabel(exportButton, dirtyFlags().profile ? 'Export working copy' : 'Export current');", "setLabel(exportButton, 'Export profile');")
app=app.replace('  wire();\n','  wire();\n  installTooltips();\n',1)
(ui/'app.js').write_text(app,encoding='utf-8')
css=(ROOT/'ui/app.css').read_text(encoding='utf-8')
with (ui/'app.css').open('a',encoding='utf-8') as f:f.write('\n'+css[css.index('/* Compact, aligned application header;'):])
host=(ROOT/'package/panel/panel-host.css').read_text()
with (ui/'panel-host.css').open('a') as f:f.write('\n'+host[host.index('/* Keep the stock host frame'):])
shutil.copy2(ROOT/'ui/version.js',ui/'version.js')
module=ROOT/'build/wasm/panel-polish-a320/FSCameraStudio.wasm'
if module.exists():shutil.copy2(module,OUT/'modules/FSCameraStudio.wasm')
entries=[dict(path=p.relative_to(OUT).as_posix(),size=p.stat().st_size,date=p.stat().st_mtime_ns//100+116444736000000000)
         for p in sorted(OUT.rglob('*')) if p.is_file() and p.name not in ['manifest.json','layout.json']]
(OUT/'layout.json').write_text(json.dumps({'content':entries},indent=2))
(OUT.parent/'staged.json').write_text(json.dumps(dict(status='staged-not-installed',liveVerified=False,
    baseline='0.1.1-test.20261011.3',moduleBuilt=module.exists(),files=inventory(OUT)),indent=2))
print(OUT)
