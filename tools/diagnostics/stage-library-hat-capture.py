"""Stage the verified library layout and hat capture iteration. Install only with MSFS closed."""
from pathlib import Path
import hashlib,json,shutil,subprocess,sys
from datetime import datetime,timezone
root=Path(__file__).resolve().parents[2]
review=root/'build/review/library-hat-capture';candidate=review/'fs-camera-studio'
base=root/'package/validated-host';prior_path=root/'build/review/controller-picker/installed.json'
target=root/'build/package/Packages/fs-camera-studio'
community=Path.home()/'AppData/Roaming/Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'
def inventory(folder):
 return {p.relative_to(folder).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.rglob('*')) if p.is_file()}
prior=json.loads(prior_path.read_text())
if '--install' not in sys.argv:
 frozen=json.loads((review/'native-source-freeze.json').read_text())
 for name,digest in frozen.items():
  assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,'Native source changed since build: '+name
 host=dict(baseline='0.2.0-library-hat-capture',liveVerified=False,files=inventory(base))
 (root/'package/validated-host.json').write_text(json.dumps(host,indent=2)+'\n')
 assert not candidate.exists(),'Use a new output directory for another candidate'
 shutil.copytree(base,candidate)
 (candidate/'modules').mkdir(exist_ok=True)
 wasm=root/'build/wasm/library-hat-capture/FSCameraStudio.wasm'
 assert wasm.is_file(),'Build the current native module first'
 shutil.copy2(wasm,candidate/'modules/FSCameraStudio.wasm')
 entries=[dict(path=p.relative_to(candidate).as_posix(),size=p.stat().st_size,date=p.stat().st_mtime_ns//100+116444736000000000) for p in sorted(candidate.rglob('*')) if p.is_file() and p.name not in ['manifest.json','layout.json']]
 (candidate/'layout.json').write_text(json.dumps({'content':entries},indent=2))
 sourceFiles={}
 for folder in ['src','tests','ui','package/validated-host']:
  sourceFiles.update({folder+'/'+k:v for k,v in inventory(root/folder).items()})
 receipt=dict(version=(root/'VERSION').read_text().strip(),localIteration='library-hat-capture',status='staged',liveVerified=False,nativeModule='rebuilt from frozen sources; native-source-freeze.json',files=inventory(candidate),sourceFiles=sourceFiles)
 (review/'staged.json').write_text(json.dumps(receipt,indent=2))
 print('Staged library layout and hat capture fixes:',candidate)
else:
 processes=subprocess.run(['tasklist.exe','/FO','CSV','/NH'],capture_output=True,text=True,check=True).stdout
 assert not any(name.lower() in processes.lower() for name in ['FlightSimulator2024.exe','FlightSimulator.exe']),'MSFS must be closed'
 assert target.resolve()==community.resolve(),'Community target changed'
 assert target.resolve().is_relative_to(root/'build'),'Target must stay in checkout'
 receipt=json.loads((review/'staged.json').read_text())
 assert inventory(candidate)==receipt['files'],'Candidate changed'
 assert inventory(target)==prior['files'],'Installed baseline changed'
 for name,digest in receipt['sourceFiles'].items():
  assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,'Source changed: '+name
 assert set(prior['files']).issubset(receipt['files']),'Unexpected removed files'
 backup=root/'build/rollback'/('before-library-hat-capture-'+datetime.now().strftime('%Y%m%d-%H%M%S'))/'fs-camera-studio'
 shutil.copytree(target,backup)
 assert inventory(backup)==prior['files'],'Backup verification failed'
 shutil.copytree(candidate,target,dirs_exist_ok=True)
 assert inventory(community)==receipt['files'],'Installed verification failed'
 receipt.update(status='installed-awaiting-live-test',backup=str(backup),installedUtc=datetime.now(timezone.utc).isoformat(),allInstalledHashesVerified=True)
 (review/'installed.json').write_text(json.dumps(receipt,indent=2))
 print('Installed and verified',len(receipt['files']),'files. Backup:',backup)
