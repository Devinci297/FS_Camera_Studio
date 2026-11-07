"""Exercise the shipping setup EXE in an isolated fake Community folder and a fake X-Plane 12 folder.

Shipping EXE: requires Windows, MSFS and X-Plane closed, and no existing installer-managed FS Camera Studio.
--test-harness: the harness setup has its own app ID and folder (%LOCALAPPDATA%\\FS Camera Studio Installer Test),
so it runs beside a real installation, with the simulators open or closed.
Never changes the real Community package, the real X-Plane folder or MSFS profile storage.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
import uuid
import winreg
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--version', default='0.6.0')
parser.add_argument('--live-receipt', default='build/review/hat-release/installed.json')
parser.add_argument('--test-harness', action='store_true')
args = parser.parse_args()
VERSION = args.version
HARNESS = args.test_harness
SETUP = ROOT / (f'build/installer-harness/{VERSION}/FS-Camera-Studio-{VERSION}-Setup.exe' if HARNESS else f'dist/{VERSION}/FS-Camera-Studio-{VERSION}-Setup.exe')
INFO = json.loads((SETUP.parent / 'BUILD-INFO.json').read_text(encoding='utf-8'))
CONTROL = Path(os.environ['LOCALAPPDATA']) / ('FS Camera Studio Installer Test' if HARNESS else 'FS Camera Studio')
assert not CONTROL.exists(), 'Existing setup support directory: do not run isolated integration tests over it.'
live_package = ROOT / 'build/package/Packages/fs-camera-studio'
# A development package and its receipt, when this clone has them: proof that the tests never write the live package.
LIVE_CHECK = live_package.is_dir() and (ROOT / args.live_receipt).is_file()


def inventory(path):
    return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob('*') if p.is_file()}


baseline = inventory(live_package) if LIVE_CHECK else None
if LIVE_CHECK:
    assert baseline == json.loads((ROOT/args.live_receipt).read_text(encoding='utf-8'))['files']
else:
    print('No development package receipt here: skipping the live-package check.')
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


def run(label, executable=SETUP, location=community, success=True, extra=()):
    args = [str(executable), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
            '/NOICONS', '/LOG=' + str(test / (label + '.log'))]
    if executable == SETUP and location is not None:
        args += ['/COMMUNITY=' + str(location)]
    if executable == SETUP:
        args += list(extra)
    proc = subprocess.run(args, timeout=90)
    assert (proc.returncode == 0) == success, (label, proc.returncode, test / (label+'.log'))
    results.append({'test': label, 'exitCode': proc.returncode, 'passed': True})


try:
    run('fresh-install')
    updater = CONTROL / 'Updater' / VERSION / 'FSCSUpdater.exe'
    assert hashlib.sha256(updater.read_bytes()).hexdigest() == INFO['updaterSha256']
    scheme = INFO['updaterProtocol']
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, 'Software\\Classes\\' + scheme + r'\shell\open\command') as key:
        command = winreg.QueryValueEx(key, '')[0]
    assert str(updater) in command and '"%1"' in command
    record = (CONTROL / 'installation.ini').read_text(encoding='utf-8-sig')
    assert 'Simulators=msfs' in record and 'Community=' + str(community) in record
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
    # X-Plane 12, in a fake X-Plane folder (X-Plane.exe and Resources\plugins), never the real one.
    xplane = test / 'X-Plane 12'
    plugins = xplane / 'Resources/plugins'
    (plugins / 'OtherPlugin').mkdir(parents=True)
    (plugins / 'OtherPlugin/keep.txt').write_text('untouched')
    (xplane / 'X-Plane.exe').write_bytes(b'')
    xtarget = plugins / 'XPCameraStudio'

    def xrun(label, components='xplane', root=xplane, success=True, community_location=None):
        run(label, location=community_location, success=success,
            extra=['/COMPONENTS=' + components, '/XPLANE=' + str(root)])

    def uninstall(label):
        run(label, executable=uninstaller)
        for attempt in range(50):
            if not uninstaller.exists(): break
            time.sleep(.1)
        assert not uninstaller.exists()

    xrun('xplane-only-install')
    record = (CONTROL / 'installation.ini').read_text(encoding='utf-8-sig')
    assert 'Simulators=xplane' in record and 'XPlane=' + str(xplane) in record
    assert inventory(xtarget) == INFO['xplanePluginFiles']
    assert not target.exists(), 'X-Plane only: nothing goes to the Community folder'
    assert (plugins / 'OtherPlugin/keep.txt').read_text() == 'untouched'
    # An upgrade backs the plugin up, then installs it into a clean folder (no stale files).
    (xtarget / 'stale.txt').write_text('from an older build')
    (xtarget / 'ui/app.js').write_text('// older test build')
    previous_plugin = inventory(xtarget)
    xrun('xplane-upgrade-clean')
    assert inventory(xtarget) == INFO['xplanePluginFiles']
    xbackups = list((CONTROL / 'Backups').glob('*/XPCameraStudio'))
    assert len(xbackups) == 1 and inventory(xbackups[0]) == previous_plugin
    uninstall('xplane-uninstall')
    assert not xtarget.exists()
    assert (plugins / 'OtherPlugin/keep.txt').read_text() == 'untouched'
    run('reject-wrong-xplane-folder', location=None, success=False,
        extra=['/COMPONENTS=xplane', '/XPLANE=' + str(test)])
    xtarget.mkdir()
    (xtarget / 'keep.txt').write_text('someone else')
    xrun('reject-unrecognized-plugin-folder', success=False)
    assert (xtarget / 'keep.txt').read_text() == 'someone else'
    (xtarget / 'keep.txt').unlink()
    xtarget.rmdir()
    # Both simulators in one run, then one uninstall removes both.
    xrun('both-simulators-install', components='msfs,xplane', community_location=community)
    assert inventory(target) == INFO['packageFiles']
    assert inventory(xtarget) == INFO['xplanePluginFiles']
    # Exercise the production C# argument builder against the installer-written record, then repair silently.
    core_test = ROOT / 'build/updater-tests/UpdaterTests.exe'
    assert core_test.exists(), 'Run tools/test-updater.ps1 before installer validation'
    generated = subprocess.check_output([str(core_test), '--setup-args', str(CONTROL / 'installation.ini')], text=True).strip()
    assert '/VERYSILENT' in generated and '/COMPONENTS=msfs,xplane' in generated
    repair = subprocess.run('"' + str(SETUP) + '" ' + generated + ' /LOG="' + str(test / 'automatic-repair.log') + '"', timeout=90)
    assert repair.returncode == 0 and inventory(target) == INFO['packageFiles'] and inventory(xtarget) == INFO['xplanePluginFiles']
    results.append({'test': 'automatic-updater-repair', 'exitCode': repair.returncode, 'passed': True})
    uninstall('both-simulators-uninstall')
    assert not xtarget.exists() and not (target.exists() and inventory(target))
    if target.exists(): target.rmdir()
    assert (community / 'unrelated-addon/keep.txt').read_text() == 'untouched'
    # A running X-Plane blocks setup (a named sleeper, never X-Plane itself).
    xp_source = test / 'FakeXPlane.cs'
    xp_source.write_text('class Program { static void Main() { System.Threading.Thread.Sleep(60000); } }')
    xp_exe = test / ('FSCS-Installer-Test-XPlane.exe' if HARNESS else 'X-Plane.exe')
    subprocess.run([r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe', '/nologo', '/target:winexe',
                    '/out:' + str(xp_exe), str(xp_source)], check=True)
    fake_xp = subprocess.Popen([str(xp_exe)], creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        time.sleep(.5)
        xrun('reject-running-xplane', success=False)
        assert not xtarget.exists()
    finally:
        fake_xp.terminate()
        fake_xp.wait(timeout=10)
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
    if LIVE_CHECK:
        assert inventory(live_package) == baseline, 'Live package changed!'
    # The protocol registration and versioned helper are removed by uninstall.
    assert not updater.exists()
    assert not (CONTROL / 'installation.ini').exists()
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, 'Software\\Classes\\' + scheme)
    except FileNotFoundError:
        pass
    else:
        key.Close()
        raise AssertionError('Updater protocol survived uninstall')
    # Preserve test evidence; relocate only the support directory created by this test.
    assert CONTROL.resolve() == (Path(os.environ['LOCALAPPDATA']) / CONTROL.name).resolve()
    assert not (CONTROL / 'Installer/unins000.exe').exists()
    shutil.move(str(CONTROL), str(test / 'installer-support-after-uninstall'))
    (test / 'results.json').write_text(json.dumps({'passed': True, 'testHarness': HARNESS, 'checks': results,
        'livePackageUnchanged': True if LIVE_CHECK else 'not checked', 'profileFixtureUnchanged': True}, indent=2),
        encoding='utf-8')
    print('PASS:', len(results), 'installer scenarios. Evidence:', test)
except Exception:
    print('FAILED: evidence at', test, '; any installed test copy is at', target)
    raise
