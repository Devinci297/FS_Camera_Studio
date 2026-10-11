"""Build a shareable, offline Windows setup from an explicitly verified package.

No SDK compile or simulator writes occur. Inno Setup 6.7.3 is a build-only tool.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(folder.rglob('*')) if p.is_file()}


def build(args):
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-test\.\d{8}\.\d+)?', args.version):
        raise ValueError('Use a release ID such as 0.2.0 or 0.2.0-test.20261011.1')
    package = (ROOT / args.package).resolve()
    receipt = json.loads((ROOT / args.receipt).read_text(encoding='utf-8-sig'))
    actual = inventory(package)
    if actual != receipt['files']:
        raise ValueError('Package differs from the verified receipt. Build and validate a new receipt first.')
    layout = json.loads((package / 'layout.json').read_text(encoding='utf-8-sig'))
    expected = set(actual) - {'layout.json', 'manifest.json'}
    if {entry['path'] for entry in layout['content']} != expected:
        raise ValueError('Layout does not list exactly the package payload.')
    for entry in layout['content']:
        path = package / entry['path']
        if not path.resolve().is_relative_to(package) or path.stat().st_size != entry['size']:
            raise ValueError('Invalid layout entry: ' + entry['path'])
    for name in actual:
        if '..' in Path(name).parts or any(c in name for c in '"\r\n;'):
            raise ValueError('Unsafe package filename: ' + name)
        if name not in {'layout.json', 'manifest.json'} and not name.startswith(('html_ui/', 'InGamePanels/', 'modules/')):
            raise ValueError('Unexpected distributable file: ' + name)
    manifest = json.loads((package / 'manifest.json').read_text(encoding='utf-8-sig'))
    stage = ROOT / ('build/installer-harness-stage' if args.test_harness else 'build/installer') / args.version
    output = ROOT / ('build/installer-harness' if args.test_harness else 'dist') / args.version
    stage.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    payload = stage / 'payload'
    if payload.exists() and set(inventory(payload)) != set(actual):
        raise ValueError('Unexpected stale payload files. Use a new build ID.')
    shutil.copytree(package, payload, dirs_exist_ok=True)
    assert inventory(payload) == actual
    shutil.copy2(ROOT / 'installer/CameraStudio.iss', stage / 'CameraStudio.iss')
    shutil.copy2(ROOT / 'installer/TESTER-GUIDE.txt', stage / 'TESTER-GUIDE.txt')
    shutil.copy2(ROOT / 'output/pdf/FS-Camera-Studio-Manual.pdf', stage / 'FS-Camera-Studio-Manual.pdf')
    (stage / 'AFTER-INSTALL.txt').write_text(
        'Installation complete.\n\nStart MSFS 2024 and open Camera Studio from the toolbar.\n'
        'Airport creation: Camera library + > Create airport cameras.\n'
        'WASD follows your view; Q/E changes height; Shift boosts movement.\n'
        'Ctrl+S saves. Bindings are customizable.\n\n'
        'The tester guide is in Start > FS Camera Studio.\n'
        'Uninstall through Windows Settings > Apps. Saved profiles are preserved.\n'
        'Build: ' + args.version + '\n', encoding='utf-8')
    notices = 'FS Camera Studio - third-party notices\n\n'
    notices += 'This package includes nlohmann/json:\n\n'
    notices += (ROOT / 'third_party/nlohmann/LICENSE.MIT').read_text(encoding='utf-8')
    (stage / 'THIRD-PARTY-NOTICES.txt').write_text(notices, encoding='utf-8')
    info = dict(product='FS Camera Studio', buildId=args.version, channel='tester' if '-test.' in args.version else 'release',
                packageVersion=manifest['package_version'], minimumSimulatorVersion=manifest['minimum_game_version'],
                builtUtc=datetime.now(timezone.utc).isoformat(),
                sourceReceipt=Path(args.receipt).name, sourceTrial=Path(args.receipt).parent.name,
                packageFiles=actual, packageFileCount=len(actual),
                liveSimulatorVerified=bool(receipt.get('liveVerified', False)),
                installerSigned=False, testHarness=args.test_harness)
    (stage / 'BUILD-INFO.json').write_text(json.dumps(info, indent=2)+'\n', encoding='utf-8')
    numeric = args.version.split('-')[0] + '.' + (args.version.rsplit('.', 1)[1] if '-test.' in args.version else '0')
    (stage / 'release.iss').write_text(
        f'#define ReleaseVersion "{args.version}"\n#define NumericVersion "{numeric}"\n'
        f'#define ReleaseOutput "{output}"\n#define StageRoot "{stage}"\n' +
        ('#define SimulatorProcessName "FSCS-Installer-Test-Sim.exe"\n' if args.test_harness else ''), encoding='utf-8-sig')
    entries, verifies = [], []
    for name, sha in actual.items():
        relative = name.replace('/', '\\')
        directory = str(Path(relative).parent)
        dest = '{app}' if directory == '.' else '{app}\\' + directory
        entries.append(f'Source: "{{#StageRoot}}\\payload\\{relative}"; DestDir: "{dest}"; Flags: ignoreversion')
        verifies.append(f"  if GetSHA256OfFile(ExpandConstant('{{app}}\\{relative}')) <> '{sha}' then RaiseException('Installed file verification failed: {relative}');")
    (stage / 'payload.iss').write_text('\n'.join(entries)+'\n', encoding='utf-8-sig')
    (stage / 'verify.iss').write_text('procedure VerifyPayload;\nbegin\n'+'\n'.join(verifies)+'\nend;\n', encoding='utf-8-sig')
    compiler = (ROOT / args.compiler).resolve()
    if not compiler.is_file():
        raise ValueError('Inno Setup compiler not found. Run tools/get-installer-compiler.ps1 or pass --compiler.')
    subprocess.run([str(compiler), '/Qp', str(stage / 'CameraStudio.iss')], check=True)
    exe = output / f'FS-Camera-Studio-{args.version}-Setup.exe'
    for name in ['TESTER-GUIDE.txt', 'THIRD-PARTY-NOTICES.txt', 'BUILD-INFO.json', 'FS-Camera-Studio-Manual.pdf']:
        shutil.copy2(stage / name, output / name)
    files = [exe.name, 'TESTER-GUIDE.txt', 'THIRD-PARTY-NOTICES.txt', 'BUILD-INFO.json', 'FS-Camera-Studio-Manual.pdf']
    checksums = ''.join(hashlib.sha256((output / name).read_bytes()).hexdigest()+'  '+name+'\n' for name in files)
    (output / 'SHA256SUMS.txt').write_text(checksums, encoding='ascii')
    archive = output / f'FS-Camera-Studio-{args.version}-Testers.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for name in files + ['SHA256SUMS.txt']:
            bundle.write(output / name, name)
    with zipfile.ZipFile(archive) as bundle:
        assert bundle.testzip() is None
    (output / (archive.name + '.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n', encoding='ascii')
    print(f'Built {exe}\nShare {archive}\nPackage: {len(actual)} files, byte-identical to the verified receipt.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='0.1.1-test.20261011.1')
    parser.add_argument('--package', default='build/release/0.1.1-test.20261011.1/fs-camera-studio')
    parser.add_argument('--receipt', default='build/release/0.1.1-test.20261011.1/staged.json')
    parser.add_argument('--compiler', default='build/installer-tools/inno/ISCC.exe')
    parser.add_argument('--test-harness', action='store_true', help='Internal lifecycle test build, always under build/, never dist/. Uses a named test process guard.')
    build(parser.parse_args())
