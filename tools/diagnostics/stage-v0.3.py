"""Freeze release inputs, then stage the reviewed host and rebuilt 0.3.0 module.

Run --freeze immediately before the native/WASM builds. Staging never installs.
"""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
version = (root / 'VERSION').read_text().strip()
assert version == '0.3.0', 'This recipe is for release 0.3.0'
review = root / 'build/release' / version
base = root / 'package/validated-host'
out = review / 'fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

def native_inputs():
    files = {}
    for folder in ['src', 'third_party', 'tests']:
        files.update({folder + '/' + k: v for k, v in inventory(root / folder).items()})
    for name in ['VERSION', 'CMakeLists.txt', 'CMakePresets.json', 'tools/build.ps1']:
        files[name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
    return files

review.mkdir(parents=True, exist_ok=True)
freeze = review / 'native-source-freeze.json'
if '--freeze' in sys.argv:
    freeze.write_text(json.dumps(native_inputs(), indent=2) + '\n')
    print('Frozen native release inputs:', freeze)
    sys.exit(0)

assert native_inputs() == json.loads(freeze.read_text()), 'Sources changed since freeze; rebuild and retest'
prior = json.loads((root / 'package/validated-host.json').read_text())
assert inventory(base) == prior['files'], 'Reviewed host differs from receipt'
assert not out.exists(), 'Release output already exists; review it before replacing anything'
wasm = root / 'build/wasm/release-0.3.0/FSCameraStudio.wasm'
assert wasm.is_file() and wasm.stat().st_mtime_ns >= freeze.stat().st_mtime_ns, 'Rebuild the frozen native module first'
assert "'0.3.0'" in (base / 'html_ui/InGamePanels/FSCameraStudio/version.js').read_text()
shutil.copytree(base, out)
(out / 'modules').mkdir(exist_ok=True)
shutil.copy2(wasm, out / 'modules/FSCameraStudio.wasm')
manifest = json.loads((out / 'manifest.json').read_text())
manifest['package_version'] = version
entries = [dict(path=p.relative_to(out).as_posix(), size=p.stat().st_size,
                date=p.stat().st_mtime_ns // 100 + 116444736000000000)
           for p in sorted(out.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
manifest['total_package_size'] = str(sum(entry['size'] for entry in entries))
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'layout.json').write_text(json.dumps({'content': entries}, indent=2) + '\n')
(review / 'staged.json').write_text(json.dumps(dict(version=version,
    baseline=prior['baseline'], liveVerified=False, nativeSourceFiles=native_inputs(),
    files=inventory(out)), indent=2) + '\n')
print('Release staged:', out)
