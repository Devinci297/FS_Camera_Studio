"""Isolate the airport-era native module while retaining the user-confirmed loading UI."""
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
baseline = root / 'build/package/Packages/fs-camera-studio'
failed = root / 'build/review/airport-loading-crash-20261011/failed-package'
candidate = root / 'build/diagnostic/airport-module-only/fs-camera-studio'
review = root / 'build/review/airport-module-only-20261011'

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def inventory(folder):
    return {p.relative_to(folder).as_posix(): sha(p) for p in sorted(folder.rglob('*')) if p.is_file()}

restored = json.loads((root / 'build/review/airport-loading-crash-20261011/rollback.json').read_text(encoding='utf-8'))
trial = json.loads((root / 'build/review/airport-test-20261011/installed.json').read_text(encoding='utf-8'))
assert inventory(baseline) == restored['files'], 'Installed baseline changed.'
assert inventory(failed) == trial['files'], 'Preserved failed package changed.'
assert not candidate.exists(), 'Inspect existing candidate before restaging.'
shutil.copytree(baseline, candidate)
shutil.copy2(failed / 'modules/FSCameraStudio.wasm', candidate / 'modules/FSCameraStudio.wasm')
layout = json.loads((candidate / 'layout.json').read_text(encoding='utf-8'))
for entry in layout['content']:
    p = candidate / entry['path']
    if entry['path'].replace('\\', '/') == 'modules/FSCameraStudio.wasm':
        entry['size'] = p.stat().st_size
        entry['date'] = p.stat().st_mtime_ns // 100 + 116444736000000000
    assert entry['size'] == p.stat().st_size, entry['path']
(candidate / 'layout.json').write_text(json.dumps(layout, indent=2), encoding='utf-8')
files = inventory(candidate)
changed = sorted(name for name in files if files[name] != restored['files'][name])
assert changed == ['layout.json', 'modules/FSCameraStudio.wasm'], changed
review.mkdir(parents=True, exist_ok=True)
(review / 'staged.json').write_text(json.dumps({
    'status': 'staged-not-installed', 'createdUtc': datetime.now(timezone.utc).isoformat(),
    'candidate': str(candidate), 'baselineLiveResult': 'User confirmed rollback loads',
    'changed': changed, 'files': files, 'baselineFiles': restored['files'],
    'purpose': 'Isolate airport-era native module from the newer UI; airport controls remain absent.',
    'liveVerified': False,
}, indent=2), encoding='utf-8')
print(f'Staged and verified {len(files)} files. Only changes: {changed}. Installed package untouched.')
