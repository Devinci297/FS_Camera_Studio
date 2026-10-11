"""Stage the tested host baseline plus the release changes, without installing."""
import hashlib,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def inventory(folder):
    return {p.relative_to(folder).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.rglob('*')) if p.is_file()}
baseline=ROOT/'package/validated-host'
prior=json.loads((ROOT/'package/validated-host.json').read_text(encoding='utf-8'))
assert inventory(baseline)==prior['files'],'Installed baseline changed; review before staging'
out=ROOT/'build/release/0.1.1-test.20261011.1/fs-camera-studio'
shutil.copytree(baseline,out,dirs_exist_ok=True)
ui=out/'html_ui/InGamePanels/FSCameraStudio'
for file in ['version.js','updates.js']:shutil.copy2(ROOT/'ui'/file,ui/file)
for file in ['main.html','mini.html']:
    text=(ui/file).read_text(encoding='utf-8')
    text=text.replace('<script src="app.js"></script>','<script src="app.js"></script>\n<script src="version.js"></script>\n<script src="updates.js"></script>')
    (ui/file).write_text(text,encoding='utf-8')
css=(ROOT/'ui/app.css').read_text(encoding='utf-8')
with (ui/'app.css').open('a',encoding='utf-8') as f:f.write('\n'+css[css.index('/* Release information'):])
(out/'modules').mkdir(exist_ok=True)
shutil.copy2(ROOT/'build/wasm/release-0.1.1/FSCameraStudio.wasm',out/'modules/FSCameraStudio.wasm')
manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8'));manifest['package_version']='0.1.1'
(out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
entries=[]
for p in sorted(out.rglob('*')):
    if p.is_file() and p.name not in ['manifest.json','layout.json']:
        entries.append(dict(path=p.relative_to(out).as_posix(),size=p.stat().st_size,date=p.stat().st_mtime_ns//100+116444736000000000))
(out/'layout.json').write_text(json.dumps({'content':entries},indent=2),encoding='utf-8')
receipt=out.parent/'staged.json'
receipt.write_text(json.dumps(dict(files=inventory(out),liveVerified=False,baseline='airport-controls-20261011',status='staged-not-installed'),indent=2),encoding='utf-8')
assert inventory(baseline)==prior['files']
print(out)
