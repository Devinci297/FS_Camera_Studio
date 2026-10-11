"""Stage movement/zoom bindings on the known-loading UI, without installing."""
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path
from movement_ui_patch import patch_app, patch_bridge

root = Path(__file__).resolve().parents[2]
baseline = root / 'build/package/Packages/fs-camera-studio'
candidate = root / 'build/diagnostic/movement-bindings/fs-camera-studio'
review = root / 'build/review/movement-bindings-20261011'
prior = json.loads((root / 'build/review/mouse-range-20261011/installed.json').read_text(encoding='utf-8'))

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

assert inventory(baseline) == prior['files'], 'Installed baseline changed.'
assert not candidate.exists(), 'Inspect existing candidate before restaging.'
shutil.copytree(baseline, candidate)
shutil.copy2(root / 'build/wasm/movement-bindings-20261011/FSCameraStudio.wasm', candidate / 'modules/FSCameraStudio.wasm')
for name, transform in [('app.js',patch_app),('bridge.js',patch_bridge)]:
    path = candidate / 'html_ui/InGamePanels/FSCameraStudio' / name
    path.write_text(transform(path.read_text(encoding='utf-8')), encoding='utf-8')
layout = json.loads((candidate / 'layout.json').read_text(encoding='utf-8'))
for entry in layout['content']:
    p = candidate / entry['path']
    entry['size'] = p.stat().st_size
    entry['date'] = p.stat().st_mtime_ns // 100 + 116444736000000000
(candidate / 'layout.json').write_text(json.dumps(layout, indent=2), encoding='utf-8')
files = inventory(candidate)
changed = sorted(name for name in files if files[name] != prior['files'][name])
assert changed == ['html_ui/InGamePanels/FSCameraStudio/app.js','html_ui/InGamePanels/FSCameraStudio/bridge.js','layout.json','modules/FSCameraStudio.wasm'], changed
review.mkdir(parents=True,exist_ok=True)
(review / 'staged.json').write_text(json.dumps({
    'status':'staged-not-installed','createdUtc':datetime.now(timezone.utc).isoformat(),
    'candidate':str(candidate),'changed':changed,'files':files,'baselineFiles':prior['files'],
    'purpose':'Editable movement/boost/zoom defaults; keyboard precision speeds; observe MSFS zoom commands for wheel zoom.',
    'liveVerified':False,
    'limitation':'Wheel requires MSFS zoom assignments and event delivery, pending in-sim verification. Full airport UI remains excluded.'
},indent=2),encoding='utf-8')
print(f'Staged {len(files)} files; only changes: {changed}. Installed package untouched.')
