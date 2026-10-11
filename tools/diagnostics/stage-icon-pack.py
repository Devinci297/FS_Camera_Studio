"""Apply the shared icon renderer to the verified .5 host; optionally install while MSFS is closed."""
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
base = root / 'build/review/input-release-fix'
review = root / 'build/review/icon-pack'
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
    for name in ['icons.js', 'version.js']:
        shutil.copy2(root / 'ui' / name, panel / name)
    shutil.copytree(root / 'ui/icons', panel / 'icons', dirs_exist_ok=True)
    with (panel / 'app.css').open('a', encoding='utf-8') as css:
        css.write('\n#mc-sim .mc-icon-choice > svg.mc-icon{width:32px!important;height:32px!important}\n')
    source = (root / 'ui/app.js').read_text(encoding='utf-8')
    app = (panel / 'app.js').read_text(encoding='utf-8')
    def function(text, name):
        start = re.search(r'^  function ' + name + r'\(', text, re.M).start()
        end = text.index('\n  }', start) + 5
        return start, end, text[start:end]
    for name in ['setIcon', 'viewIcon', 'decorateName', 'chooseEntityIcon', 'renderCameraTools']:
        start, end, _ = function(app, name)
        app = app[:start] + function(source, name)[2] + app[end:]
    app = app.replace("['Paste settings', 'folder-input'", "['Paste settings', 'paste'")
    (panel / 'app.js').write_text(app, encoding='utf-8')
    for name in ['main.html', 'mini.html']:
        page = panel / name
        page.write_text(page.read_text(encoding='utf-8').replace(
            'id="mc-save" class="mc-primary" disabled><i data-icon="check"',
            'id="mc-save" class="mc-primary" disabled><i data-icon="save"'), encoding='utf-8')
    entries = [dict(path=p.relative_to(candidate).as_posix(), size=p.stat().st_size,
                    date=p.stat().st_mtime_ns // 100 + 116444736000000000)
               for p in sorted(candidate.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
    (candidate / 'layout.json').write_text(json.dumps({'content': entries}, indent=2))
    (review / 'staged.json').write_text(json.dumps(dict(status='staged', version='0.1.1-test.20261011.6',
        files=inventory(candidate), liveVerified=False), indent=2))
    print('Staged icon update:', candidate)
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
    backup = root / 'build/rollback' / ('before-icon-pack-' + datetime.now().strftime('%Y%m%d-%H%M%S')) / 'fs-camera-studio'
    shutil.copytree(target, backup)
    assert inventory(backup) == prior['files']
    shutil.copytree(candidate, target, dirs_exist_ok=True)
    assert inventory(community) == receipt['files']
    receipt.update(status='installed-awaiting-live-test', backup=str(backup),
                   installedUtc=datetime.now(timezone.utc).isoformat(), allInstalledHashesVerified=True)
    (review / 'installed.json').write_text(json.dumps(receipt, indent=2))
    print('Installed and hash-verified', len(receipt['files']), 'files. Rollback:', backup)
