"""Stage the global Auto switch fix on the verified .6 host; install only with MSFS closed."""
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
base = root / 'build/review/icon-pack'
review = root / 'build/review/global-auto-switch'
candidate = review / 'fs-camera-studio'
target = root / 'build/package/Packages/fs-camera-studio'
community = Path.home() / 'AppData/Roaming/Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

prior = json.loads((base / 'installed.json').read_text())
if '--install' not in sys.argv:
    assert inventory(base / 'fs-camera-studio') == prior['files'], 'Baseline changed'
    shutil.copytree(base / 'fs-camera-studio', candidate, dirs_exist_ok=True)
    panel = candidate / 'html_ui/InGamePanels/FSCameraStudio'
    shutil.copy2(root / 'ui/version.js', panel / 'version.js')
    app = (panel / 'app.js').read_text(encoding='utf-8')
    old = 'Select the nearest loaded airport camera now and as the aircraft moves. Manual camera selection, movement or right-drag turns this off.'
    new = 'Shared across all cameras and categories. Select the nearest airport camera automatically. Camera changes and manual movement pause switching for 3 seconds; releasing camera control pauses it until a camera is active again.'
    assert old in app
    (panel / 'app.js').write_text(app.replace(old,new), encoding='utf-8')
    shutil.copy2(root / 'build/wasm/global-auto-switch/FSCameraStudio.wasm', candidate / 'modules/FSCameraStudio.wasm')
    entries = [dict(path=p.relative_to(candidate).as_posix(), size=p.stat().st_size,
                    date=p.stat().st_mtime_ns // 100 + 116444736000000000)
               for p in sorted(candidate.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
    (candidate / 'layout.json').write_text(json.dumps({'content': entries}, indent=2))
    (review / 'staged.json').write_text(json.dumps(dict(status='staged', version='0.1.1-test.20261011.7',
        files=inventory(candidate), liveVerified=False), indent=2))
    print('Staged global Auto switch update:', candidate)
else:
    processes = subprocess.run(['tasklist.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True, check=True)
    assert 'FlightSimulator2024.exe' not in processes.stdout and 'FlightSimulator.exe' not in processes.stdout, 'Close MSFS before installing'
    assert community.resolve() == target.resolve() and target.resolve().is_relative_to(root / 'build')
    assert inventory(target) == prior['files'], 'Installed package changed'
    receipt = json.loads((review / 'staged.json').read_text())
    assert inventory(candidate) == receipt['files'], 'Candidate changed'
    assert set(prior['files']).issubset(receipt['files'])
    for entry in json.loads((candidate / 'layout.json').read_text())['content']:
        assert (candidate / entry['path']).stat().st_size == entry['size']
    backup = root / 'build/rollback' / ('before-global-auto-switch-' + datetime.now().strftime('%Y%m%d-%H%M%S')) / 'fs-camera-studio'
    shutil.copytree(target, backup)
    assert inventory(backup) == prior['files']
    shutil.copytree(candidate, target, dirs_exist_ok=True)
    assert inventory(community) == receipt['files']
    receipt.update(status='installed-awaiting-live-test', backup=str(backup),
                   installedUtc=datetime.now(timezone.utc).isoformat(), allInstalledHashesVerified=True)
    (review / 'installed.json').write_text(json.dumps(receipt, indent=2))
    print('Installed and hash-verified', len(receipt['files']), 'files. Rollback:', backup)
