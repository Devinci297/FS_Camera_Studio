"""Offline checks: never reads or writes the simulator's live work directory."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('fenix', ROOT/'tools/diagnostics/prepare-fenix-calibration.py')
FENIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FENIX)
FIXTURES = ROOT/'build/review/input-release-fix'


class CalibratedStoreTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT/'build/review')
        root = Path(self.temp.name)
        self.work = root/'fake-work'
        self.work.mkdir()
        self.review = root/'review'
        self.patch = patch.multiple(FENIX, ROOT=root, WORK=self.work, REVIEW=self.review)
        self.patch.start()
        for name in FENIX.SLOTS:
            (self.work/name).write_bytes((FIXTURES/('source-'+name)).read_bytes())
        (self.work/'additional-user-file.json').write_text('{"untouched":true}')
        FENIX.prepare()

    def tearDown(self):
        self.patch.stop()
        self.temp.cleanup()

    def test_preservation_and_rotations(self):
        receipt = json.loads((self.review/'fenix-calibration-receipt.json').read_text())
        before = FENIX.decode((self.work/receipt['sourceSlot']).read_bytes())
        after = FENIX.decode((self.review/'candidate-store.json').read_bytes())
        prior = next(p for p in before['profiles'] if p['current']['profileId'] == FENIX.PROFILE_ID)
        current = next(p for p in after['profiles'] if p['current']['profileId'] == FENIX.PROFILE_ID)
        self.assertEqual(len(receipt['rebased']), 10)
        self.assertEqual(len(receipt['preserved']), 2)
        self.assertEqual(current['backups'], prior['backups'])
        for camera in prior['current']['cameras']:
            updated = next(c for c in current['current']['cameras'] if c['id'] == camera['id'])
            if camera['name'] in FENIX.PROTECTED:
                self.assertEqual(updated, camera)
            else:
                unchanged = copy.deepcopy(updated)
                for key in ('positionMetersFRD','orientationCameraToAnchorXYZW'):
                    unchanged[key] = camera[key]
                self.assertEqual(unchanged, camera)
            self.assertAlmostEqual(sum(v*v for v in updated['orientationCameraToAnchorXYZW']), 1, places=12)
        for camera in current['current']['cameras'][12:]:
            self.assertEqual(camera['mode'], 'aircraftExterior')
            self.assertEqual(camera['aircraftTracking'], 'off')
            self.assertFalse(camera['interaction'])
        current['current'] = prior['current']
        after['txn'] = before['txn']
        self.assertEqual(after, before)

    def test_install_one_slot_with_complete_backup(self):
        original = FENIX.inventory(self.work)
        with patch.object(FENIX, 'stopped') as stopped:
            FENIX.install(False)
            self.assertEqual(FENIX.inventory(self.work), original)
            FENIX.install(True)
            self.assertEqual(stopped.call_count, 3)
        installed = json.loads((self.review/'fenix-calibration-installed.json').read_text())
        self.assertEqual(FENIX.inventory(Path(installed['backupDirectory'])), original)
        expected = dict(original)
        expected[installed['targetSlot']] = installed['candidateHash']
        self.assertEqual(FENIX.inventory(self.work), expected)

    def test_refuse_running_sim_and_changed_source(self):
        original = FENIX.inventory(self.work)
        with patch.object(FENIX, 'stopped', side_effect=RuntimeError('running')):
            with self.assertRaisesRegex(RuntimeError, 'running'):
                FENIX.install(True)
        self.assertEqual(FENIX.inventory(self.work), original)
        slot = self.work/FENIX.SLOTS[0]
        slot.write_bytes(slot.read_bytes()+b' ')
        changed = FENIX.inventory(self.work)
        with patch.object(FENIX, 'stopped'):
            with self.assertRaisesRegex(RuntimeError, 'Live store changed'):
                FENIX.install(True)
        self.assertEqual(FENIX.inventory(self.work), changed)

    def test_crc_rejects_payload_edit(self):
        raw = (self.review/'candidate-store.json').read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'CRC mismatch'):
            FENIX.decode(raw.replace(b'External views', b'External fake!', 1))


if __name__ == '__main__':
    unittest.main()
