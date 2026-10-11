"""Exercise the shipping setup EXE in an isolated fake Community folder.

Requires Windows, MSFS closed, and no existing installer-managed FS Camera Studio.
Never changes the real Community package or MSFS profile storage.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
import uuid
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--version', default='0.1.1-test.20261011.2')
parser.add_argument('--live-receipt', default='build/review/airport-tracking-fix/installed.json')
parser.add_argument('--test-harness', action='store_true')
args = parser.parse_args()
VERSION = args.version
HARNESS = args.test_harness
SETUP = ROOT / (f'build/installer-harness/{VERSION}/FS-Camera-Studio-{VERSION}-Setup.exe' if HARNESS else f'dist/{VERSION}/FS-Camera-Studio-{VERSION}-Setup.exe')
INFO = json.loads((SETUP.parent / 'BUILD-INFO.json').read_text(encoding='utf-8'))
CONTROL = Path(os.environ['LOCALAPPDATA']) / 'FS Camera Studio'
assert not CONTROL.exists(), 'Existing setup support directory: do not run isolated integration tests over it.'
live_package = ROOT / 'build/package/Packages/fs-camera-studio'


def inventory(path):
    return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob('*') if p.is_file()}


baseline = inventory(live_package)
assert baseline == json.loads((ROOT/args.live_receipt).read_text(encoding='utf-8'))['files']
test = ROOT / 'build/installer-tests' / str(uuid.uuid4())
community = test / 'Packages/Community'
community.mkdir(parents=True)
target = community / 'fs-camera-studio'
(community / 'unrelated-addon').mkdir()
(community / 'unrelated-addon/keep.txt').write_text('untouched')
profile = test / 'WASM/work/profile.json'
profile.parent.mkdir(parents=True)
profile.write_text('{"testProfile":"keep"}')
results = []


def run(label, executable=SETUP, location=community, success=True):
    args = [str(executable), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            '/NOICONS', '/LOG=' + str(test / (label + '.log'))]
    if executable == SETUP and location is not None:
        args += ['/COMMUNITY=' + str(location)]
    proc = subprocess.run(args, timeout=90)
    assert (proc.returncode == 0) == success, (label, proc.returncode, test / (label+'.log'))
    results.append({'test': label, 'exitCode': proc.returncode, 'passed': True})


try:
    run('fresh-install')
    assert inventory(target) == INFO['packageFiles']
    uninstaller = CONTROL / 'Installer/unins000.exe'
    assert uninstaller.is_file()
    assert (CONTROL / 'FS-Camera-Studio-Manual.pdf').read_bytes() == (SETUP.parent / 'FS-Camera-Studio-Manual.pdf').read_bytes()
    # Unknown files survive upgrades and uninstall, while all shipped files are repaired.
    (target / 'tester-notes.txt').write_text('Preserve this file.')
    (target / 'html_ui/InGamePanels/FSCameraStudio/app.js').write_text('// older test build')
    previous = inventory(target)
    run('upgrade-repair')
    for name, sha in INFO['packageFiles'].items():
        assert hashlib.sha256((target / name).read_bytes()).hexdigest() == sha
    backups = list((CONTROL / 'Backups').glob('*/fs-camera-studio'))
    assert len(backups) == 1 and inventory(backups[0]) == previous
    run('uninstall-preserves-extra-files', executable=uninstaller)
    for attempt in range(50):
        if not uninstaller.exists(): break
        time.sleep(.1)
    assert inventory(target) == {'tester-notes.txt': hashlib.sha256(b'Preserve this file.').hexdigest()}
    assert not uninstaller.exists()
    assert (community / 'unrelated-addon/keep.txt').read_text() == 'untouched'
    assert profile.read_text() == '{"testProfile":"keep"}'
    # The unrecognized remaining folder is not silently adopted on a new install.
    run('reject-unrecognized-folder', success=False)
    (target / 'tester-notes.txt').unlink()
    target.rmdir()
    run('reject-wrong-folder', location=test, success=False)
    # Real developer package is a junction; detection must reject it without writing.
    real_link = Path(os.environ['APPDATA']) / 'Microsoft Flight Simulator 2024/Packages/Community/fs-camera-studio'
    if real_link.is_junction():
        run('autodetect-rejects-development-link', location=None, success=False)
    external = test / 'linked-source'
    external.mkdir()
    (external / 'keep.txt').write_text('Never follow the link')
    quoted_target = str(target).replace("'", "''")
    quoted_external = str(external).replace("'", "''")
    subprocess.run(['powershell.exe', '-NoProfile', '-Command',
                    f"New-Item -ItemType Junction -Path '{quoted_target}' -Target '{quoted_external}' | Out-Null"], check=True)
    run('reject-package-junction', success=False)
    assert (external / 'keep.txt').read_text() == 'Never follow the link'
    # rmdir removes only this test-created junction; no recursive traversal.
    assert target.parent.resolve() == community.resolve() and target.is_junction()
    target.rmdir()
    duplicate = test / 'Packages/Community2024/fs-camera-studio'
    duplicate.mkdir(parents=True)
    (duplicate / 'keep.txt').write_text('duplicate')
    run('reject-duplicate-copy', success=False)
    (duplicate / 'keep.txt').unlink()
    duplicate.rmdir()
    # Named sleeper exercises the process guard, without ever starting MSFS.
    fake_source = test / 'FakeSim.cs'
    fake_source.write_text('class Program { static void Main() { System.Threading.Thread.Sleep(60000); } }')
    fake_exe = test / ('FSCS-Installer-Test-Sim.exe' if HARNESS else 'FlightSimulator2024.exe')
    subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe', '/nologo', '/target:winexe',
                    '/out:' + str(fake_exe), str(fake_source)], check=True)
    fake = subprocess.Popen([str(fake_exe)], creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        time.sleep(.5)
        run('reject-running-simulator', success=False)
        assert not target.exists()
    finally:
        fake.terminate()
        fake.wait(timeout=10)
    assert inventory(live_package) == baseline, 'Live package changed!'
    # Preserve test evidence; relocate only the support directory created by this test.
    assert CONTROL.resolve() == (Path(os.environ['LOCALAPPDATA']) / 'FS Camera Studio').resolve()
    assert not (CONTROL / 'Installer/unins000.exe').exists()
    shutil.move(str(CONTROL), str(test / 'installer-support-after-uninstall'))
    (test / 'results.json').write_text(json.dumps({'passed': True, 'testHarness': HARNESS, 'checks': results,
        'livePackageUnchanged': True, 'profileFixtureUnchanged': True}, indent=2), encoding='utf-8')
    print('PASS:', len(results), 'installer scenarios. Evidence:', test)
except Exception:
    print('FAILED: evidence at', test, '; any installed test copy is at', target)
    raise
