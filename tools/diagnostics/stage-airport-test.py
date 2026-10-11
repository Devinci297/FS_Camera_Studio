"""Compose the airport test from unchanged compiled panels and fresh copy assets."""
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
installed = root / 'build/package/Packages/fs-camera-studio'
staged = root / 'build/airport-test-project/PackageSources'
candidate = root / 'build/airport-test-package/Packages/fs-camera-studio'
review = root / 'build/review/airport-test-20261011'
review.mkdir(parents=True, exist_ok=True)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def inventory(folder):
    return {p.relative_to(folder).as_posix(): sha(p) for p in sorted(folder.rglob('*')) if p.is_file()}

assert not candidate.exists(), 'Candidate already exists; inspect it before restaging.'
prior = json.loads((root / 'build/review/mouse-dispatch-once-20261011/installed-awaiting-live-test.json').read_text(encoding='utf-8'))
for entry in prior['files']:
    assert sha(installed / entry['file']) == entry['sha256'], entry['file']
for xml in (staged / 'InGamePanels').glob('*.xml'):
    assert sha(xml) == sha(root / 'build/project/PackageSources/InGamePanels' / xml.name), xml.name
shutil.copytree(installed, candidate)
for folder in ['html_ui', 'modules']:
    shutil.copytree(staged / folder, candidate / folder, dirs_exist_ok=True)
entries = []
for p in sorted(candidate.rglob('*')):
    if not p.is_file() or p.name in ['manifest.json', 'layout.json']:
        continue
    entries.append({'path': p.relative_to(candidate).as_posix(), 'size': p.stat().st_size,
                    'date': int(p.stat().st_mtime_ns // 100) + 116444736000000000})
(candidate / 'layout.json').write_text(json.dumps({'content': entries}, indent=2), encoding='utf-8')
source_files = {}
for folder in ['src', 'ui', 'package']:
    for p in sorted((root / folder).rglob('*')):
        if p.is_file() and 'node_modules' not in p.parts:
            source_files[p.relative_to(root).as_posix()] = sha(p)
receipt = {'status': 'staged-awaiting-install', 'createdUtc': datetime.now(timezone.utc).isoformat(),
           'candidate': str(candidate), 'files': inventory(candidate), 'sourceFiles': source_files,
           'nativeTests': {'passed': 102, 'failed': 0, 'checks': 5045},
           'packageCompiler': 'Exit 1 with no output; reused unchanged, hash-verified compiled SPB definitions.',
           'liveVerified': False}
(review / 'staged.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(f'Staged {len(receipt["files"])} files; {len(entries)} layout entries; WASM {sha(candidate / "modules/FSCameraStudio.wasm")}')
