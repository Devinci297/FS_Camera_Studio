"""Reproduce the release using the reviewed simulator host, preserving native pointer routing."""
import hashlib
import json
import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[2]
version = (root / 'VERSION').read_text().strip()
assert version == '0.2.0'
base = root / 'package/validated-host'
out = root / 'build/release' / version / 'fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

prior = json.loads((root / 'package/validated-host.json').read_text())
assert inventory(base) == prior['files'], 'Reviewed host differs from receipt'
shutil.copytree(base, out, dirs_exist_ok=True)
panel = out / 'html_ui/InGamePanels/FSCameraStudio'
shutil.copy2(root / 'ui/version.js', panel / 'version.js')
(out / 'modules').mkdir(exist_ok=True)
shutil.copy2(root / 'build/wasm/release-0.2.0/FSCameraStudio.wasm', out / 'modules/FSCameraStudio.wasm')
manifest = json.loads((out / 'manifest.json').read_text())
manifest['package_version'] = version
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
entries = [dict(path=p.relative_to(out).as_posix(), size=p.stat().st_size,
                date=p.stat().st_mtime_ns // 100 + 116444736000000000)
           for p in sorted(out.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
(out / 'layout.json').write_text(json.dumps({'content': entries}, indent=2))
(out.parent / 'staged.json').write_text(json.dumps(dict(version=version,
    baseline=prior['baseline'], liveVerified=False, files=inventory(out)), indent=2))
print('Release staged:', out)
