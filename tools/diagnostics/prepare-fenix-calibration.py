"""Prepare a calibrated Fenix profile; install only with explicit --install and MSFS stopped.

Preparation is read-only outside the repository. Installation validates both source
slots, backs up the entire work store, then atomically replaces only the older slot.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import uuid
import zlib
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
REVIEW = ROOT / 'build/review/input-release-fix'
WORK = Path.home() / 'AppData/Roaming/Microsoft Flight Simulator 2024/WASM/MSFS2024/fs-camera-studio/work'
PROFILE_ID = '01c4f28c-e7b0-450e-ab27-2e77a3cc70fa'
SLOTS = ('fscs-store-a.json', 'fscs-store-b.json')
PROTECTED = {'Pilot - normal', 'Pilot - wide cockpit'}
STARTER_ANGLES = {'Pilot - landing': (-3, 0), 'Left window': (0, -70),
    'Right cockpit': (-5, 65), 'Captain displays': (-24, 0),
    'Center displays': (-23, 28), 'Main panel overview': (-20, 15),
    'Glareshield': (-8, 23), 'Overhead': (55, 25),
    'Pedestal': (-58, 38), 'Left side controls': (-40, -60)}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def decode(raw):
    envelope = json.loads(raw)
    require(envelope['format'] == 'fs-camera-studio-store' and envelope['version'] == 1, 'Unknown store format')
    require(raw.endswith(b'}'), 'Store must end with its envelope brace')
    body = raw.split(b',"payload":', 1)[1][:-1]
    require(f'{zlib.crc32(body):08x}' == envelope['crc'], 'Exact-payload CRC mismatch')
    require(json.loads(body) == envelope['payload'], 'Payload envelope mismatch')
    return envelope['payload']


def encode(payload):
    body = json.dumps(payload, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode('utf-8')
    return b'{"format":"fs-camera-studio-store","version":1,"crc":"' + f'{zlib.crc32(body):08x}'.encode() + b'","payload":' + body + b'}'


def multiply(a, b):
    x, y, z, w = a
    X, Y, Z, W = b
    q = [w*X+x*W+y*Z-z*Y, w*Y-x*Z+y*W+z*X,
         w*Z+x*Y-y*X+z*W, w*W-x*X-y*Y-z*Z]
    length = math.sqrt(sum(v*v for v in q))
    return [v/length for v in q]


def pbh(pitch, heading):
    p, h = math.radians(pitch)/2, math.radians(heading)/2
    return multiply([0, 0, math.sin(h), math.cos(h)], [0, math.sin(p), 0, math.cos(p)])


def same_quaternion(a, b):
    return min(max(abs(x-y) for x,y in zip(a,b)), max(abs(x+y) for x,y in zip(a,b))) < 1e-9


def calibrated_profile(original):
    profile = copy.deepcopy(original)
    require(profile['aircraftMatch']['modelKey'] == 'fnx_32x', 'Unexpected aircraft model')
    require(profile.get('builtinTemplate') == 'a320-eyepoint-v1', 'Unexpected starter template')
    require(not any(c['name'] == 'External views' for c in profile['categories']), 'External views already exists; inspect before repeating')
    pilot = next(c for c in profile['cameras'] if c['name'] == 'Pilot - normal')
    require(any(abs(v) > .1 for v in pilot['positionMetersFRD']), 'Pilot has not been calibrated')
    changes, preserved = [], []
    for camera in profile['cameras']:
        name = camera['name']
        angles = STARTER_ANGLES.get(name)
        untouched = angles and max(abs(v) for v in camera['positionMetersFRD']) < 1e-10 and same_quaternion(camera['orientationCameraToAnchorXYZW'], pbh(*angles))
        if name in PROTECTED or not untouched:
            preserved.append({'id': camera['id'], 'name': name, 'reason': 'user-adjusted or not an untouched starter'})
            continue
        before = {k: copy.deepcopy(camera[k]) for k in ('positionMetersFRD', 'orientationCameraToAnchorXYZW')}
        camera['positionMetersFRD'] = copy.deepcopy(pilot['positionMetersFRD'])
        camera['orientationCameraToAnchorXYZW'] = multiply(pilot['orientationCameraToAnchorXYZW'], camera['orientationCameraToAnchorXYZW'])
        changes.append({'id': camera['id'], 'name': name, 'before': before,
                        'after': {k: camera[k] for k in before}})
    require(changes, 'No untouched starter cameras found; inspect before continuing')
    category_id = str(uuid.uuid4())
    category = {'id': category_id, 'name': 'External views', 'icon': 'plane', 'color': '#bb9cf5', 'cameraIds': []}
    # Approximate FRD coordinates around the SIMOBJECT origin, never cockpit offsets.
    exterior = [('Front left quarter', [32,-26,-7], 55), ('Front right quarter', [32,26,-7], 55),
                ('Left wing', [-3,-26,-5], 65), ('Right wing', [-3,26,-5], 65),
                ('Tail - chase', [-35,0,-8], 55), ('Nose - head on', [36,0,-4], 55),
                ('Above - overview', [-5,0,-38], 65), ('Rear left quarter', [-30,-25,-8], 55)]
    used = {c.get('viewId') for c in profile['cameras']}
    for name, position, fov in exterior:
        view_id = 1
        while view_id in used:
            view_id += 1
        used.add(view_id)
        f, r, d = position
        # Aim at the aircraft's SIMOBJECT origin; flight review may refine framing.
        pitch, heading = math.degrees(math.atan2(d, math.hypot(f,r))), math.degrees(math.atan2(-r,-f))
        camera_id = str(uuid.uuid4())
        camera = {'id': camera_id, 'categoryId': category_id, 'name': name, 'icon': 'plane',
                  'mode': 'aircraftExterior', 'requiredCapabilities': ['fov'], 'viewId': view_id,
                  'favorite': name in ('Front left quarter', 'Tail - chase'),
                  'positionMetersFRD': position, 'orientationCameraToAnchorXYZW': pbh(pitch,heading),
                  'projection': {'kind':'simulatorFovRadians','axis':'adapterDefined','value':math.radians(fov)},
                  'transition': {'kind':'smoothstep3','durationSeconds':1.2,'delaySeconds':0},
                  'interaction': False, 'trackingPolicy':'off','aircraftTracking':'off',
                  'effects': {'enabled':False}, 'temporaryLookPolicy':'resetOnRecall',
                  'levelHorizon':False,'seedDefinitionName':None}
        profile['cameras'].append(camera)
        category['cameraIds'].append(camera_id)
    category['defaultCameraId'] = category['cameraIds'][0]
    profile['categories'].append(category)
    profile['revision'] += 1
    return profile, changes, preserved


def pretty(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n', encoding='utf-8')


def prepare():
    raw = {name: (WORK/name).read_bytes() for name in SLOTS}
    stores = {name: decode(data) for name,data in raw.items()}
    source_slot = max(stores, key=lambda name: stores[name]['txn'])
    target_slot = next(name for name in SLOTS if name != source_slot)
    require(stores[source_slot]['txn'] > stores[target_slot]['txn'], 'Ambiguous equal transaction slots')
    before = stores[source_slot]
    after = copy.deepcopy(before)
    records = [r for r in after['profiles'] if r['current']['profileId'] == PROFILE_ID]
    require(len(records) == 1, 'Target profile missing or duplicated')
    original = copy.deepcopy(records[0]['current'])
    profile, changes, preserved = calibrated_profile(original)
    records[0]['current'] = profile
    after['txn'] += 1
    raw_candidate = encode(after)
    require(decode(raw_candidate) == after, 'Candidate serialization mismatch')
    # The only state changes are this profile's current document and transaction.
    restored = copy.deepcopy(after)
    restored['txn'] = before['txn']
    next(r for r in restored['profiles'] if r['current']['profileId'] == PROFILE_ID)['current'] = original
    require(restored == before, 'Unexpected settings/bindings/backups/profile modification')
    for name,data in raw.items():
        require((WORK/name).read_bytes() == data, 'Store changed during preparation; rerun after saving')
    REVIEW.mkdir(parents=True, exist_ok=True)
    pretty(REVIEW/'fenix-a320-calibrated-profile.json', profile)
    pretty(REVIEW/'fenix-a320-calibrated-export.json', {'format':'fs-camera-studio-profile','version':2,'workingCopy':False,'profile':profile})
    (REVIEW/'candidate-store.json').write_bytes(raw_candidate)
    for name,data in raw.items():
        (REVIEW/('source-'+name)).write_bytes(data)
    receipt = {'preparedUtc':datetime.now(timezone.utc).isoformat(), 'workDirectory':str(WORK),
               'profileId':PROFILE_ID, 'sourceSlot':source_slot, 'targetSlot':target_slot,
               'sourceHashes':{name:sha(data) for name,data in raw.items()},
               'sourceTransaction':before['txn'], 'candidateTransaction':after['txn'],
               'originalRevision':original['revision'], 'candidateRevision':profile['revision'],
               'candidateHash':sha(raw_candidate), 'rebased':changes, 'preserved':preserved,
               'exteriorCameraIds':profile['categories'][-1]['cameraIds'],
               'allOtherStatePreserved':True,
               'limitations':['Exterior coordinates are inferred around the SIMOBJECT origin and require live framing review.',
                              'Untouched interior view angles retain their starter composition relative to the calibrated pilot; each view needs live review.']}
    pretty(REVIEW/'fenix-calibration-receipt.json', receipt)
    print(f'Prepared txn {after["txn"]}, revision {profile["revision"]}: {len(changes)} rebased, {len(preserved)} preserved, 8 exterior cameras.')


def stopped():
    result = subprocess.run(['tasklist.exe','/FO','CSV','/NH'], capture_output=True, text=True, check=True)
    names = result.stdout.lower()
    require('flightsimulator2024.exe' not in names and 'flightsimulator.exe' not in names, 'Close MSFS before installing the profile')


def inventory(directory):
    return {p.relative_to(directory).as_posix():sha(p.read_bytes()) for p in directory.rglob('*') if p.is_file()}


def install(apply=False):
    receipt = json.loads((REVIEW/'fenix-calibration-receipt.json').read_text(encoding='utf-8'))
    require(Path(receipt['workDirectory']).resolve() == WORK.resolve(), 'Unexpected work store path')
    stopped()
    candidate = (REVIEW/'candidate-store.json').read_bytes()
    require(sha(candidate) == receipt['candidateHash'], 'Candidate changed after preparation')
    payload = decode(candidate)
    require(payload['txn'] == receipt['candidateTransaction'], 'Candidate transaction mismatch')
    target_slot = receipt['targetSlot']
    require(target_slot in SLOTS and target_slot != receipt['sourceSlot'], 'Invalid alternating target slot')
    for name in SLOTS:
        require(sha((WORK/name).read_bytes()) == receipt['sourceHashes'][name], 'Live store changed; regenerate the candidate before installing')
    if not apply:
        print('Ready: MSFS stopped, both live slots unchanged, candidate CRC and hash verified. Use --install to apply.')
        return
    require(all(not p.is_symlink() and not (hasattr(p,'is_junction') and p.is_junction()) for p in WORK.rglob('*')), 'Work directory contains a link; inspect before backing up')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    backup = ROOT/'build/rollback'/('before-fenix-calibration-'+stamp)/'work'
    before_inventory = inventory(WORK)
    shutil.copytree(WORK, backup)
    require(inventory(backup) == before_inventory and inventory(WORK) == before_inventory, 'Backup mismatch or work directory changed during backup')
    stopped()
    for name in SLOTS:
        require(sha((WORK/name).read_bytes()) == receipt['sourceHashes'][name], 'Live store changed during backup')
    temporary = WORK/(target_slot+'.'+str(uuid.uuid4())+'.tmp')
    try:
        with temporary.open('xb') as stream:
            stream.write(candidate)
            stream.flush()
            os.fsync(stream.fileno())
        require(temporary.read_bytes() == candidate, 'Temporary store verification failed')
        os.replace(temporary, WORK/target_slot)
    finally:
        if temporary.exists():
            temporary.unlink()
    expected = dict(before_inventory)
    expected[target_slot] = receipt['candidateHash']
    require(inventory(WORK) == expected, 'Post-install store inventory mismatch')
    require(decode((WORK/target_slot).read_bytes()) == payload, 'Installed CRC verification failed')
    pretty(REVIEW/'fenix-calibration-installed.json', {'installedUtc':datetime.now(timezone.utc).isoformat(),
            'backupDirectory':str(backup), 'targetSlot':target_slot,'candidateHash':receipt['candidateHash'],
            'transaction':payload['txn'],'status':'installed-awaiting-live-review'})
    print('Installed one alternating slot atomically. Complete work backup:', backup)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--prepare', action='store_true')
    mode.add_argument('--install', action='store_true')
    args = parser.parse_args()
    if args.prepare:
        prepare()
    else:
        install(args.install)
