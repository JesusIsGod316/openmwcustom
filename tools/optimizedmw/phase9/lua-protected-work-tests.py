"""Reject startup-only work, broken transport and hidden loading activity."""
import csv
from pathlib import Path
import tempfile
import unittest

import lua_protected_work as work
from indirect_work import positive_counts


class ProtectedWork(unittest.TestCase):
    def make(self, root, values, flags=None):
        flags = flags or {}
        with (root/'v3-frame.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('frame', 'epoch_ms'))
            writer.writerows((i, i*1000) for i in range(len(values)))
        with (root/'benchmark-context.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('frame', 'begin_flags', 'end_flags', 'observed_flags'))
            writer.writerows((i, 0, 0, flags.get(i, 0)) for i in range(len(values)))
        with (root/'p9-lua-protected-work.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('frame', 'epoch_ms', 'enabled', 'row_seq', *work.TOTALS))
            writer.writerows((i, i*1000, 1, i+1, value, 1, 0) for i, value in enumerate(values))
        (root/'p9-lua-protected-work.csv.writer-status.txt').write_text(
            'normal_finish=1\noutput_ok=1\nrows_dropped=0\ninitialization_failed=0\n')
        (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows='+str(len(values))+'\n')
        return {'start': 1000, 'movement': 5000, 'finish': 9000}

    def test_startup_reuse_cannot_qualify_candidate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [100]*9)
            self.assertFalse(positive_counts(work.interval_work(root, windows), 'reused_total'))
            self.make(root, [0, 0, 0, 2, 4, 7, 10, 13, 999])
            result = work.interval_work(root, windows)
            self.assertEqual(result['phases']['moving']['counts']['reused_total'], 9)

    def test_loading_work_is_retained_but_excluded_from_normal_intervals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0, 0, 0, 0, 0, 200, 200, 200, 200], {5: 1})
            result = work.interval_work(root, windows)
            self.assertFalse(positive_counts(result, 'reused_total'))
            self.assertEqual(result['rows'], 9)

    def test_transport_and_tail_truncation_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0, 1, 2, 3])
            status = root/'p9-lua-protected-work.csv.writer-status.txt'
            status.write_text(status.read_text().replace('rows_dropped=0', 'rows_dropped=1'))
            with self.assertRaisesRegex(ValueError, 'transport'):
                work.interval_work(root, windows)
            self.make(root, [0, 1, 2, 3])
            (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows=5\n')
            with self.assertRaisesRegex(ValueError, 'truncated'):
                work.interval_work(root, windows)

    def test_reset_counts_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, [0, 9, 2, 3])
            with self.assertRaisesRegex(ValueError, 'Reset'):
                work.interval_work(root, windows)


if __name__ == '__main__':
    unittest.main()
