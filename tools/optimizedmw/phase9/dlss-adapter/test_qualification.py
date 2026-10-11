"""Offline tampered-receipt checks against a finished native cohort; no GPU or game launch."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import unittest

from qualify_adapter import checked_run


class ReceiptRejection(unittest.TestCase):
    positive = None
    negative = None

    def copied(self, root):
        destination = Path(self.temp.name) / 'run'
        destination.mkdir()
        for relative in ('INPUTS.json', 'adapter-evidence.jsonl', 'Desktop/stdout.log',
                         'Desktop/stderr.log', 'Desktop/DESKTOP-RECEIPT.json'):
            target = destination / relative
            target.parent.mkdir(exist_ok=True)
            shutil.copy2(root / relative, target)
        return destination

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='optimizedmw-dlss-receipt-')

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def edit_json(path, edit):
        value = json.loads(path.read_text(encoding='utf-8-sig'))
        edit(value)
        path.write_text(json.dumps(value))

    @staticmethod
    def edit_rows(path, edit):
        rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
        edit(rows)
        path.write_text(''.join(json.dumps(row) + '\n' for row in rows))

    def test_actual_and_negative_cohort(self):
        checked_run(self.positive, False)
        checked_run(self.negative, True)

    def test_exit_without_actual_completion_marker(self):
        path = self.copied(self.positive)
        self.edit_rows(path / 'adapter-evidence.jsonl', lambda rows:
            rows.__setitem__(slice(None), [row for row in rows if row['check'] != 'adapter_complete']))
        with self.assertRaises(AssertionError):
            checked_run(path, False)

    def test_wrong_reset_or_missing_exposure_proof(self):
        path = self.copied(self.positive)
        original = (path / 'adapter-evidence.jsonl').read_text()
        for field, value in (('reset', False), ('exact_exposure_pixel', False)):
            with self.subTest(field=field):
                (path / 'adapter-evidence.jsonl').write_text(original)
                self.edit_rows(path / 'adapter-evidence.jsonl', lambda rows:
                    next(row for row in rows if row['check'] == 'real_dlss_evaluated').__setitem__(field, value))
                with self.assertRaises(AssertionError):
                    checked_run(path, False)

    def test_missing_resource_retirement(self):
        path = self.copied(self.positive)
        self.edit_rows(path / 'adapter-evidence.jsonl', lambda rows:
            rows.__setitem__(slice(None), [row for row in rows if row['check'] != 'feature_retirement_resources_released']))
        with self.assertRaises(AssertionError):
            checked_run(path, False)

    def test_visible_window_or_failed_exit(self):
        path = self.copied(self.positive)
        receipt = path / 'Desktop/DESKTOP-RECEIPT.json'
        original = receipt.read_text()
        for field, value in (('input_desktop_owned_visible_windows', [{'pid': 1}]), ('exit_code', 1)):
            with self.subTest(field=field):
                receipt.write_text(original)
                self.edit_json(receipt, lambda row: row.__setitem__(field, value))
                with self.assertRaises(AssertionError):
                    checked_run(path, False)

    def test_negative_requires_actual_rejection_marker(self):
        path = self.copied(self.negative)
        self.edit_rows(path / 'Desktop/stdout.log', lambda rows:
            rows.__setitem__(slice(None), [row for row in rows if row['check'] != 'negative_no_evaluation']))
        with self.assertRaises(AssertionError):
            checked_run(path, True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--positive', type=Path, required=True)
    parser.add_argument('--negative', type=Path, required=True)
    args = parser.parse_args()
    ReceiptRejection.positive, ReceiptRejection.negative = args.positive, args.negative
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ReceiptRejection)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
