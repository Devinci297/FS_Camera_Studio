"""Stage wider panels/global airport auto over the verified .2 host, without installing."""
import hashlib, json, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/review/airport-tracking-fix'
OUT = ROOT / 'build/review/global-airport-auto/fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

assert inventory(BASE / 'fs-camera-studio') == json.loads((BASE / 'staged.json').read_text())['files']
shutil.copytree(BASE / 'fs-camera-studio', OUT, dirs_exist_ok=True)
ui = OUT / 'html_ui/InGamePanels/FSCameraStudio'
source = (ROOT / 'ui/app.js').read_text(encoding='utf-8')
app = (ui / 'app.js').read_text(encoding='utf-8')
for start, end in [('  function renderCameraTools()', '  function dockCamera()'),
                   ('  function cameraRow(', '  function '),
                   ('  function dockViewButton(', '  function ')]:
    a = source.index(start); b = source.index(end, a + len(start))
    x = app.index(start); y = app.index(end, x + len(start))
    app = app[:x] + source[a:b] + app[y:]
app = app.replace('Follow the nearest camera while in an airport view', 'Switch to the nearest loaded airport camera from any view')
(ui / 'app.js').write_text(app, encoding='utf-8')
css = (ROOT / 'ui/app.css').read_text(encoding='utf-8')
with (ui / 'app.css').open('a', encoding='utf-8') as f:
    f.write('\n' + css[css.index('/* Roomier panels;'):])
shutil.copy2(ROOT / 'ui/version.js', ui / 'version.js')
shutil.copy2(ROOT / 'build/wasm/global-airport-auto/FSCameraStudio.wasm', OUT / 'modules/FSCameraStudio.wasm')
# Only replace the newly compiled main-window definition; preserve the known-loading mini host.
shutil.copy2(ROOT / 'build/compiled-global-airport-auto/Packages/fs-camera-studio/InGamePanels/fscs-camera-studio.spb',
             OUT / 'InGamePanels/fscs-camera-studio.spb')
entries = [dict(path=p.relative_to(OUT).as_posix(), size=p.stat().st_size,
                date=p.stat().st_mtime_ns // 100 + 116444736000000000)
           for p in sorted(OUT.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
(OUT / 'layout.json').write_text(json.dumps({'content': entries}, indent=2), encoding='utf-8')
(OUT.parent / 'staged.json').write_text(json.dumps(dict(status='staged-not-installed', liveVerified=False,
    baseline='0.1.1-test.20261011.2', files=inventory(OUT)), indent=2), encoding='utf-8')
print(OUT)
