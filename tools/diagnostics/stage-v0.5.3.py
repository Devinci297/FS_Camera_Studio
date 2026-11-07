"""Freeze release inputs, refresh the reviewed host from a compiled package, then stage the 0.5.3 release.

Order (MSFS closed for the compile step; staging never installs):
  python tools/diagnostics/stage-v0.5.3.py --freeze
  tools\\build.ps1 -ModuleOnly -OutputDirectory build/wasm/release-0.5.3
  tools\\package.ps1 -Compile -Wasm build/wasm/release-0.5.3/FSCameraStudio.wasm -OutputDirectory build/release-0.5.3-compile
  python tools/diagnostics/stage-v0.5.3.py --host build/release-0.5.3-compile/Packages/fs-camera-studio
  python tools/diagnostics/stage-v0.5.3.py

--host replaces package/validated-host with the compiled panel host (everything except modules/) and rewrites
package/validated-host.json. Commit both with the release so the shipped host stays reviewable.
"""
import hashlib
import json
import shutil
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
version = (root / 'VERSION').read_text().strip()
assert version == '0.5.3', 'This recipe is for release 0.5.3'
review = root / 'build/release' / version
base = root / 'package/validated-host'
out = review / 'fs-camera-studio'
wasm = root / 'build/wasm/release-0.5.3/FSCameraStudio.wasm'
freeze = review / 'native-source-freeze.json'


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


def check_versions(folder):
    assert "'" + version + "'" in (folder / 'html_ui/InGamePanels/FSCameraStudio/version.js').read_text(), \
        'Panel version.js does not report ' + version
    assert json.loads((folder / 'manifest.json').read_text())['package_version'] == version, \
        'Package manifest is not ' + version


review.mkdir(parents=True, exist_ok=True)
if '--freeze' in sys.argv:
    freeze.write_text(json.dumps(native_inputs(), indent=2) + '\n')
    print('Frozen native release inputs:', freeze)
    sys.exit(0)

assert native_inputs() == json.loads(freeze.read_text()), 'Sources changed since freeze; rebuild and retest'

if '--host' in sys.argv:
    compiled = (root / sys.argv[sys.argv.index('--host') + 1]).resolve()
    assert compiled.is_relative_to(root / 'build'), 'Use a compiled package under build/'
    for name in ['InGamePanels/fscs-camera-studio.spb', 'InGamePanels/fscs-camera-mini.spb', 'manifest.json',
                 'html_ui/InGamePanels/FSCameraStudio/main.html', 'html_ui/InGamePanels/FSCameraStudio/mini.html']:
        assert (compiled / name).is_file(), 'Compiled package is missing ' + name
    check_versions(compiled)
    module = compiled / 'modules/FSCameraStudio.wasm'
    assert module.is_file() and module.read_bytes() == wasm.read_bytes(), 'Compile with the frozen release module'
    shutil.rmtree(base)
    shutil.copytree(compiled, base, ignore=lambda folder, names: ['modules'] if Path(folder) == compiled else [])
    (root / 'package/validated-host.json').write_text(json.dumps(dict(
        baseline=version + '-release', liveVerified=False, files=inventory(base)), indent=2) + '\n')
    print('Reviewed host refreshed from', compiled)
    sys.exit(0)

prior = json.loads((root / 'package/validated-host.json').read_text())
assert prior['baseline'] == version + '-release', 'Refresh the reviewed host with --host first'
assert inventory(base) == prior['files'], 'Reviewed host differs from receipt'
assert not out.exists(), 'Release output already exists; review it before replacing anything'
assert wasm.is_file() and wasm.stat().st_mtime_ns >= freeze.stat().st_mtime_ns, 'Rebuild the frozen native module first'
check_versions(base)
shutil.copytree(base, out)
(out / 'modules').mkdir(exist_ok=True)
shutil.copy2(wasm, out / 'modules/FSCameraStudio.wasm')
entries = [dict(path=p.relative_to(out).as_posix(), size=p.stat().st_size,
                date=p.stat().st_mtime_ns // 100 + 116444736000000000)
           for p in sorted(out.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
manifest = json.loads((out / 'manifest.json').read_text())
manifest['total_package_size'] = str(sum(entry['size'] for entry in entries))
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'layout.json').write_text(json.dumps({'content': entries}, indent=2) + '\n')
(review / 'staged.json').write_text(json.dumps(dict(version=version,
    baseline=prior['baseline'], liveVerified=False, nativeSourceFiles=native_inputs(),
    files=inventory(out)), indent=2) + '\n')
print('Release staged:', out)
