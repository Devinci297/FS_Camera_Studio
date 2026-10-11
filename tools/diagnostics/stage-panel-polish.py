"""Stage panel, import, sound and motion updates on the verified .8 host; install only with MSFS closed."""
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[2]
base = root / 'build/review/readable-icons-import'
review = root / 'build/review/panel-polish'
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
    for name in ['icons.js', 'version.js', 'save-feedback.js']:
        shutil.copy2(root / 'ui' / name, panel / name)
    source = (root / 'ui/app.js').read_text(encoding='utf-8')
    app = (panel / 'app.js').read_text(encoding='utf-8')
    def function(text, name):
        start = re.search(r'^  (?:async )?function ' + name + r'\(', text, re.M).start()
        end = text.index('\n  }', start) + 5
        return start, end, text[start:end]
    for name in ['chooseEntityIcon','renderTree','cameraRow','renderDetails','dockViewButton','renderDock','startMouseLook','openDialog','airportDialog','renderAirportNearby','importProfile','buildAxes','layoutMiniCategories','renderSettings','applySettings','renderCameraTools','saveAdjustedViews','saveChanges']:
        start,end,_=function(app,name)
        app=app[:start]+function(source,name)[2]+app[end:]
    for name in ['entityIcon','wireNudgeHold','addExternalMotionViews','renderMotionSettings','walkaroundDialog','renderSaveFeedbackSettings','applyMainOpacity','renderMainOpacitySettings']:
        app=app.replace('  function viewIcon(',function(source,name)[2]+'\n  function viewIcon(',1)
    app=app.replace('airportPreviews: {},','airportPreviews: {}, airportExpanded: true,')
    app=app.replace('    miniCategory: null,','    miniCategory: null,\n    walkaround: {running:false,categoryId:null,intervalSeconds:5,loop:true},')
    app=app.replace('    S.airports = p.airports || S.airports;','    S.airports = p.airports || S.airports;\n    S.walkaround = p.walkaround || S.walkaround;')
    app=app.replace('    Snapshot: applySnapshot,','    Snapshot: applySnapshot,\n    WalkaroundChanged(p) { S.walkaround=p; renderCameraTools(); renderMotionSettings(current()); },')
    marker="    $('mc-add').addEventListener"
    old=next(line for line in app.splitlines() if line.startswith(marker))
    app=app.replace(old,next(line for line in source.splitlines() if line.startswith(marker)),1)
    start=source.index("    $('mc-view-type').addEventListener('click'")
    end=source.index("    $('mc-view-id').addEventListener",start)
    old=next(line for line in app.splitlines() if "$('mc-view-type').addEventListener('change'" in line)
    app=app.replace(old,source[start:end].rstrip(),1)
    for name in ['main.html','mini.html']:
        page=panel/name
        html=page.read_text()
        html=re.sub(r'<select id="mc-view-type">.*?</select>','<button type="button" id="mc-view-type">Cockpit</button>',html)
        if 'id="mc-mouse-release"' not in html:
            row=next(line for line in (root/'ui/index.html').read_text().splitlines() if 'id="mc-mouse-release"' in line)
            anchor=next(line for line in html.splitlines() if 'id="mc-retain-look"' in line)
            html=html.replace(anchor,row+'\n'+anchor)
        for key in ['mc-setting-mouse-enabled','mc-setting-mouse-sensitivity','mc-setting-mouse-smoothing','mc-setting-mouse-invert','mc-setting-airport-radius']:
            if 'id="'+key+'"' not in html:
                row=next(line for line in (root/'ui/index.html').read_text().splitlines() if 'id="'+key+'"' in line)
                anchor=next(line for line in html.splitlines() if 'id="mc-setting-look-speed"' in line)
                html=html.replace(anchor,row+'\n'+anchor)
        page.write_text(html)
    app=app.replace("flyby: { reference: 'Aircraft relative - meters'","flyby: { reference: 'World fixed fly-by'")
    for key in ['mc-setting-mouse-enabled','mc-setting-mouse-invert']:
        row=next(line for line in source.splitlines() if "$('"+key+"').addEventListener" in line)
        app=app.replace("    $('mc-setting-mini-opacity').addEventListener",row+"\n    $('mc-setting-mini-opacity').addEventListener",1)
    old=next(line for line in app.splitlines() if "    [['mc-setting-look-speed', 'lookSpeedDegPerSec'" in line)
    new=next(line for line in source.splitlines() if "    [['mc-setting-airport-radius', 'airportRadiusNm'" in line)
    app=app.replace(old,new)
    (panel / 'app.js').write_text(app, encoding='utf-8')
    bridge=(panel/'bridge.js').read_text()
    bridge=bridge.replace("orbit: { available: false, reason: NOT_AVAILABLE }, flyby: { available: false, reason: NOT_AVAILABLE },","orbit: { available: true, reason: null }, flyby: { available: true, reason: null },")
    bridge=bridge.replace("        case 'RecenterLook':","        case 'StartWalkaround': event('WalkaroundChanged',Object.assign({running:true},payload)); return ack(env,'Accepted');\n        case 'StopWalkaround': event('WalkaroundChanged',{running:false,categoryId:null,intervalSeconds:5,loop:true}); return ack(env,'Completed');\n        case 'RecenterLook':")
    (panel/'bridge.js').write_text(bridge)
    shutil.copy2(root/'package/panel/panel-host.css',panel/'panel-host.css')
    with (panel / 'app.css').open('a',encoding='utf-8') as css:
        css.write('\n'+'/* Category targets fill the dock height'+(root/'ui/app.css').read_text().split('/* Category targets fill the dock height',1)[1])
    shutil.copy2(root / 'build/wasm/panel-polish/FSCameraStudio.wasm', candidate / 'modules/FSCameraStudio.wasm')
    entries = [dict(path=p.relative_to(candidate).as_posix(), size=p.stat().st_size,
                    date=p.stat().st_mtime_ns // 100 + 116444736000000000)
               for p in sorted(candidate.rglob('*')) if p.is_file() and p.name not in ['manifest.json', 'layout.json']]
    (candidate / 'layout.json').write_text(json.dumps({'content': entries}, indent=2))
    (review / 'staged.json').write_text(json.dumps(dict(status='staged', version='0.1.1-test.20261011.9',
        files=inventory(candidate), liveVerified=False), indent=2))
    print('Staged readable icon / import / tower update:', candidate)
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
    backup = root / 'build/rollback' / ('before-panel-polish-' + datetime.now().strftime('%Y%m%d-%H%M%S')) / 'fs-camera-studio'
    shutil.copytree(target, backup)
    assert inventory(backup) == prior['files']
    shutil.copytree(candidate, target, dirs_exist_ok=True)
    assert inventory(community) == receipt['files']
    receipt.update(status='installed-awaiting-live-test', backup=str(backup),
                   installedUtc=datetime.now(timezone.utc).isoformat(), allInstalledHashesVerified=True)
    (review / 'installed.json').write_text(json.dumps(receipt, indent=2))
    print('Installed and hash-verified', len(receipt['files']), 'files. Rollback:', backup)
