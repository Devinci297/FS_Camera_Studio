"""Install the staged 0.2 release into this development checkout with a verified rollback."""
import hashlib
import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
review = root / 'build/release/0.2.0'
candidate = review / 'fs-camera-studio'
target = root / 'build/package/Packages/fs-camera-studio'
community = Path.home() / 'AppData/Roaming/Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'

def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.rglob('*') if p.is_file()}

processes = subprocess.run(['tasklist.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True, check=True)
assert not any(name in processes.stdout for name in ['FlightSimulator2024.exe', 'FlightSimulator.exe']), 'Close MSFS before installing.'
assert target.resolve() == community.resolve(), 'Community target changed.'
assert target.resolve().is_relative_to(root / 'build'), 'Installation must stay in this checkout.'
receipt = json.loads((review / 'staged.json').read_text())
prior = json.loads((root / 'build/review/panel-polish/installed.json').read_text())
assert inventory(candidate) == receipt['files'], 'Staged package changed.'
assert inventory(target) == prior['files'], 'Installed package changed.'
assert set(prior['files']).issubset(receipt['files']), 'Unexpected removed package files.'
backup = root / 'build/rollback' / ('before-0.2.0-' + datetime.now().strftime('%Y%m%d-%H%M%S')) / 'fs-camera-studio'
shutil.copytree(target, backup)
assert inventory(backup) == prior['files'], 'Rollback verification failed.'
shutil.copytree(candidate, target, dirs_exist_ok=True)
assert inventory(community) == receipt['files'], 'Installed hashes differ.'
receipt.update(status='installed-awaiting-live-test', backup=str(backup),
               installedUtc=datetime.now(timezone.utc).isoformat(), allInstalledHashesVerified=True)
(review / 'installed.json').write_text(json.dumps(receipt, indent=2))
print('Installed and hash-verified', len(receipt['files']), 'files. Rollback:', backup)
