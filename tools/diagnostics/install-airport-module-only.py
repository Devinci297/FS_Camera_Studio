"""Install only the verified module-only trial, preserving its known-loading baseline."""
import hashlib
import json
import shutil
import subprocess
import argparse
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--trial', choices=['airport-module-only-20261011', 'mouse-range-20261011', 'movement-bindings-20261011', 'airport-menu-20261011', 'airport-controls-20261011'], default='airport-module-only-20261011')
args = parser.parse_args()
review = root / 'build/review' / args.trial
receipt = json.loads((review / 'staged.json').read_text(encoding='utf-8'))
candidate = Path(receipt['candidate'])
target = root / 'build/package/Packages/fs-camera-studio'
community = Path.home() / 'AppData/Roaming/Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}

processes = subprocess.run(['tasklist.exe', '/FI', 'IMAGENAME eq FlightSimulator2024.exe', '/FO', 'CSV', '/NH'],
                           capture_output=True, text=True, check=True)
assert 'FlightSimulator2024.exe' not in processes.stdout, 'MSFS is running.'
assert community.resolve() == target.resolve(), 'Community target changed.'
assert candidate.resolve().is_relative_to(root / 'build'), 'Unexpected candidate location.'
assert inventory(candidate) == receipt['files'], 'Candidate changed after staging.'
assert inventory(target) == receipt['baselineFiles'], 'Installed baseline changed.'
changed = sorted(name for name, value in receipt['files'].items() if value != receipt['baselineFiles'][name])
expected_changes = ['layout.json', 'modules/FSCameraStudio.wasm']
if args.trial == 'mouse-range-20261011': expected_changes.insert(0, 'html_ui/InGamePanels/FSCameraStudio/app.css')
if args.trial == 'movement-bindings-20261011': expected_changes[:0] = ['html_ui/InGamePanels/FSCameraStudio/app.js', 'html_ui/InGamePanels/FSCameraStudio/bridge.js']
if args.trial == 'airport-menu-20261011': expected_changes = ['html_ui/InGamePanels/FSCameraStudio/app.css', 'html_ui/InGamePanels/FSCameraStudio/app.js', 'layout.json']
if args.trial == 'airport-controls-20261011': expected_changes = ['html_ui/InGamePanels/FSCameraStudio/app.css', 'html_ui/InGamePanels/FSCameraStudio/app.js', 'html_ui/InGamePanels/FSCameraStudio/bridge.js', 'layout.json', 'modules/FSCameraStudio.wasm']
assert changed == expected_changes, changed
layout = json.loads((candidate / 'layout.json').read_text(encoding='utf-8'))
for entry in layout['content']:
    assert (candidate / entry['path']).stat().st_size == entry['size'], entry['path']
backup = root / 'build/rollback' / ('before-' + args.trial + '-' + datetime.now().strftime('%Y%m%d-%H%M%S')) / 'fs-camera-studio'
shutil.copytree(target, backup)
assert inventory(backup) == receipt['baselineFiles'], 'Backup verification failed.'
for name in changed:
    shutil.copy2(candidate / name, target / name)
assert inventory(community) == receipt['files'], 'Installed verification failed.'
receipt.update(status='installed-awaiting-live-test', installedUtc=datetime.now(timezone.utc).isoformat(),
               backup=str(backup), installed=str(community), allInstalledHashesVerified=True,
               verifiedLayoutEntries=len(layout['content']))
(review / 'installed.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(f'Installed {len(changed)} changed files; verified all {len(receipt["files"])} package hashes and {len(layout["content"])} layout entries. Backup: {backup}')
