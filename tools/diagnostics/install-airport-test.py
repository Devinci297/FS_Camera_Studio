"""Install the staged airport candidate after the caller verifies MSFS is closed."""
import hashlib
import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
review = root / 'build/review/airport-test-20261011'
receipt = json.loads((review / 'staged.json').read_text(encoding='utf-8'))
candidate = Path(receipt['candidate'])
target = root / 'build/package/Packages/fs-camera-studio'
community = Path.home() / 'AppData/Roaming/Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def inventory(folder):
    return {p.relative_to(folder).as_posix(): sha(p) for p in sorted(folder.rglob('*')) if p.is_file()}

processes = subprocess.run(['tasklist.exe', '/FI', 'IMAGENAME eq FlightSimulator2024.exe', '/FO', 'CSV', '/NH'],
                           capture_output=True, text=True, check=True)
assert 'FlightSimulator2024.exe' not in processes.stdout, 'Close MSFS before installing.'
assert target.resolve() == community.resolve(), 'Community target changed.'
assert candidate.resolve().is_relative_to(root / 'build'), 'Candidate must stay in build.'
assert inventory(candidate) == receipt['files'], 'Candidate changed after staging.'
for name, expected in receipt['sourceFiles'].items():
    assert sha(root / name) == expected, f'Source changed after staging: {name}'
assert set(inventory(target)) == set(receipt['files']), 'Unexpected installed files; inspect before replacing.'
for entry in json.loads((candidate / 'layout.json').read_text(encoding='utf-8'))['content']:
    assert (candidate / entry['path']).stat().st_size == entry['size'], entry['path']
backup = root / 'build/rollback' / ('before-airport-test-' + datetime.now().strftime('%Y%m%d-%H%M%S')) / 'fs-camera-studio'
shutil.copytree(target, backup)
assert inventory(backup) == inventory(target), 'Backup verification failed.'
shutil.copytree(candidate, target, dirs_exist_ok=True)
assert inventory(community) == receipt['files'], 'Installed hash verification failed.'
receipt.update(status='installed-awaiting-live-test', installedUtc=datetime.now(timezone.utc).isoformat(),
               backup=str(backup), installed=str(community), allInstalledHashesVerified=True)
(review / 'installed.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(f'Installed and verified {len(receipt["files"])} files through Community junction. Backup: {backup}')
