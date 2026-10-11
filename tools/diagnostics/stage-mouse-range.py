"""Preserve the loading UI, adding only the requested picker removal and mouse range fix."""
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
baseline = root / 'build/package/Packages/fs-camera-studio'
candidate = root / 'build/diagnostic/mouse-range/fs-camera-studio'
review = root / 'build/review/mouse-range-20261011'
prior = json.loads((root / 'build/review/airport-module-only-20261011/installed.json').read_text(encoding='utf-8'))
def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}
assert inventory(baseline) == prior['files'], 'Installed baseline changed.'
assert not candidate.exists(), 'Inspect existing candidate before restaging.'
shutil.copytree(baseline, candidate)
shutil.copy2(root / 'build/wasm/mouse-range-20261011/FSCameraStudio.wasm', candidate / 'modules/FSCameraStudio.wasm')
css = candidate / 'html_ui/InGamePanels/FSCameraStudio/app.css'
rule = '#mc-sim #mc-mini-panel #mc-quick-picker{display:none}'
assert rule in (root / 'ui/app.css').read_text(encoding='utf-8')
with css.open('a', encoding='utf-8') as out: out.write('\n/* Remove redundant mini category/camera picker. */\n' + rule + '\n')
layout = json.loads((candidate / 'layout.json').read_text(encoding='utf-8'))
for entry in layout['content']:
    p = candidate / entry['path']
    entry['size'] = p.stat().st_size
    entry['date'] = p.stat().st_mtime_ns // 100 + 116444736000000000
(candidate / 'layout.json').write_text(json.dumps(layout, indent=2), encoding='utf-8')
files = inventory(candidate)
changed = sorted(name for name in files if files[name] != prior['files'][name])
assert changed == ['html_ui/InGamePanels/FSCameraStudio/app.css', 'layout.json', 'modules/FSCameraStudio.wasm'], changed
review.mkdir(parents=True, exist_ok=True)
(review / 'staged.json').write_text(json.dumps({
    'status': 'staged-not-installed', 'createdUtc': datetime.now(timezone.utc).isoformat(),
    'candidate': str(candidate), 'changed': changed, 'files': files, 'baselineFiles': prior['files'],
    'purpose': 'Accept mouse counts outside joystick range; hide redundant mini picker with one CSS rule.',
    'nativeTests': {'passed':103,'failed':0,'checks':5185}, 'liveVerified':False,
}, indent=2), encoding='utf-8')
print(f'Staged {len(files)} files; only changes: {changed}. Installed package untouched.')
