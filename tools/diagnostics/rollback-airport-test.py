"""Capture the loading failure and restore the package preceding the airport trial."""
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from datetime import datetime, timezone

root = Path(__file__).resolve().parents[2]
sim = Path.home() / 'AppData/Roaming/Microsoft Flight Simulator 2024'
out = root / 'build/review/airport-loading-crash-20261011'
out.mkdir(parents=True, exist_ok=True)
for name in ['AsoboReport-Crash.txt', 'crashdata.txt', 'WASM/MSFS2024/fs-camera-studio/work/fscs-diagnostics.log']:
    p = sim / name
    if p.exists(): shutil.copy2(p, out / p.name)
receipt = json.loads((root / 'build/review/airport-test-20261011/installed.json').read_text(encoding='utf-8'))
backup = Path(receipt['backup'])
target = root / 'build/package/Packages/fs-camera-studio'
community = sim / 'Packages/Community/fs-camera-studio'
def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.rglob('*')) if p.is_file()}
prior = json.loads((root / 'build/review/mouse-dispatch-once-20261011/installed-awaiting-live-test.json').read_text(encoding='utf-8'))
expected = {p['file'].replace('\\', '/'): p['sha256'] for p in prior['files']}
assert inventory(backup) == expected, 'Previous backup verification failed.'
assert inventory(target) == receipt['files'], 'Installed package changed; inspect before rollback.'
assert community.resolve() == target.resolve()
running = subprocess.run(['tasklist.exe', '/FI', 'IMAGENAME eq FlightSimulator2024.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True, check=True)
assert 'FlightSimulator2024.exe' not in running.stdout, 'MSFS is running.'
failed = out / 'failed-package'
if not failed.exists(): shutil.copytree(target, failed)
assert inventory(failed) == receipt['files']
shutil.copytree(backup, target, dirs_exist_ok=True)
assert inventory(community) == expected, 'Rollback verification failed.'
(out / 'rollback.json').write_text(json.dumps({'restoredUtc': datetime.now(timezone.utc).isoformat(), 'restoredFrom': str(backup), 'files': expected, 'liveVerified': False, 'airportTrial': 'failed-flight-loading'}, indent=2), encoding='utf-8')
print(f'Restored and verified {len(expected)} files. Crash evidence and failed package: {out}')
