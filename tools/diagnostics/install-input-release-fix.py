"""Install the verified candidate through the existing Community junction, with rollback."""
import hashlib,json,shutil,subprocess
from datetime import datetime,timezone
from pathlib import Path
root=Path(__file__).resolve().parents[2]
review=root/'build/review/input-release-fix'
candidate=review/'fs-camera-studio'
target=root/'build/package/Packages/fs-camera-studio'
community=Path.home()/'AppData/Roaming/Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'
def inventory(folder):
    return {p.relative_to(folder).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file()}
processes=subprocess.run(['tasklist.exe','/FO','CSV','/NH'],capture_output=True,text=True,check=True)
assert 'FlightSimulator2024.exe' not in processes.stdout and 'FlightSimulator.exe' not in processes.stdout,'Close MSFS before installing.'
assert target.resolve()==community.resolve(),'Community target changed.'
assert target.resolve().is_relative_to(root/'build'),'Installation must stay in the build directory.'
receipt=json.loads((review/'staged.json').read_text())
assert receipt.get('moduleBuilt'), 'Build the module before installing.'
assert inventory(candidate)==receipt['files'],'Candidate changed after staging.'
prior=json.loads((root/'build/review/panel-polish-a320/installed.json').read_text())
assert inventory(target)==prior['files'],'Installed package changed; inspect before replacing.'
assert set(prior['files']).issubset(receipt['files']),'Unexpected removed package files.'
for entry in json.loads((candidate/'layout.json').read_text())['content']:
    assert (candidate/entry['path']).stat().st_size==entry['size'],entry['path']
backup=root/'build/rollback'/('before-input-release-fix-'+datetime.now().strftime('%Y%m%d-%H%M%S'))/'fs-camera-studio'
shutil.copytree(target,backup)
assert inventory(backup)==prior['files'],'Backup hash mismatch.'
shutil.copytree(candidate,target,dirs_exist_ok=True)
assert inventory(community)==receipt['files'],'Installed hash mismatch.'
receipt.update(status='installed-awaiting-live-test',installedUtc=datetime.now(timezone.utc).isoformat(),backup=str(backup),installed=str(community),allInstalledHashesVerified=True)
(review/'installed.json').write_text(json.dumps(receipt,indent=2))
print('Installed and verified',len(receipt['files']),'files. Rollback:',backup)
