"""Prove interval engagement cannot be supplied by startup work or broken joins."""
import csv
from pathlib import Path
import tempfile
import unittest

from indirect_work import COUNTERS, EXTENDED_COUNTERS, PREPARATION_EXTENDED_COUNTERS, interval_work, positive_counts


class IndirectWork(unittest.TestCase):
    def make(self, root, values, flags=None, counters=COUNTERS):
        flags = flags or {}
        with (root/'v3-frame.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('frame', 'epoch_ms'))
            writer.writerows((i, i * 1000) for i in range(len(values)))
        with (root/'benchmark-context.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('frame', 'begin_flags', 'end_flags', 'observed_flags'))
            writer.writerows((i, 0, 0, flags.get(i, 0)) for i in range(len(values)))
        with (root/'p9-indirect-work.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('snapshot_frame', *counters))
            writer.writerows((i, *([value] * len(counters))) for i, value in enumerate(values))
        (root/'p8g4-render.capture-status.txt').write_text(
            f'normal_finish=1\noutput_ok=1\nallocation_failed=0\nindirect_work_dropped=0\nindirect_work_rows={len(values)}\n')
        return {'start': 1000, 'movement': 5000, 'finish': 9000}

    def test_work_is_qualified_by_both_interval_endpoints(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [100, 100, 100, 102, 104, 107, 110, 113, 999])
            result = interval_work(root, windows)
            self.assertEqual(result['phases']['fixed_view']['counts']['indirect_draws'], 2)
            self.assertEqual(result['phases']['moving']['counts']['indirect_draws'], 9)
            self.assertTrue(positive_counts(result, 'opportunity_original_draws'))

    def test_startup_counts_cannot_qualify_an_inert_route(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [100] * 9)
            self.assertFalse(positive_counts(interval_work(root, windows), 'capability_draws'))

    def test_context_work_is_excluded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0, 0, 0, 5, 5, 20, 20, 20, 20], {5: 1})
            result = interval_work(root, windows)
            self.assertEqual(result['phases']['moving']['counts']['commands'], 0)

    def test_transport_loss_is_a_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0] * 9)
            path = root/'p8g4-render.capture-status.txt'
            path.write_text(path.read_text().replace('indirect_work_dropped=0', 'indirect_work_dropped=1'))
            with self.assertRaisesRegex(ValueError, 'transport'):
                interval_work(root, windows)

    def test_counter_reset_is_a_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0, 0, 3, 4, 2, 5, 6, 7, 8])
            with self.assertRaisesRegex(ValueError, 'reset'):
                interval_work(root, windows)

    def test_truncated_rows_are_a_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0] * 9)
            path = root/'p9-indirect-work.csv'
            path.write_text('\n'.join(path.read_text().splitlines()[:-1]) + '\n')
            with self.assertRaisesRegex(ValueError, 'truncated'):
                interval_work(root, windows)

    def test_extended_mechanisms_need_actual_interval_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows = self.make(root, [100, 100, 100, 102, 104, 107, 110, 113, 999], counters=EXTENDED_COUNTERS)
            result = interval_work(root, windows)
            self.assertEqual(result['schema'], 2)
            for key in ('projection_proof_reuses', 'early_admission_fallbacks', 'paging_pow_reuses'):
                self.assertEqual(result['phases']['moving']['counts'][key], 9)
                self.assertTrue(positive_counts(result, key))
            windows = self.make(root, [100] * 9, counters=EXTENDED_COUNTERS)
            self.assertFalse(positive_counts(interval_work(root, windows), 'paging_pow_reuses'))

    def test_legacy_capture_cannot_qualify_new_switches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0, 0, 0, 2, 4, 7, 10, 13, 999])
            result = interval_work(root, windows)
            self.assertEqual(result['schema'], 1)
            self.assertTrue(positive_counts(result, 'indirect_draws'))
            self.assertFalse(positive_counts(result, 'projection_proof_reuses'))

    def test_partial_new_header_is_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows = self.make(root, [0] * 9, counters=(*COUNTERS, 'projection_proof_reuses'))
            with self.assertRaisesRegex(ValueError, 'header'):
                interval_work(root, windows)

    def test_preparation_schema_and_legacy_cannot_invent_queue_starts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows = self.make(root, [0, 0, 0, 2, 4, 7, 10, 13, 999], counters=PREPARATION_EXTENDED_COUNTERS)
            result = interval_work(root, windows)
            self.assertEqual(result['schema'], 3)
            self.assertEqual(result['phases']['moving']['counts']['preload_priority_started'], 9)
            for counters in (COUNTERS, EXTENDED_COUNTERS):
                windows = self.make(root, [0, 0, 0, 2, 4, 7, 10, 13, 999], counters=counters)
                self.assertFalse(positive_counts(interval_work(root, windows), 'preload_priority_started'))


if __name__ == '__main__':
    unittest.main()
