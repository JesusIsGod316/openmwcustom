"""Reject lossy, inert or falsely exact VM-state evidence."""
import csv
from pathlib import Path
import tempfile
import unittest

from lua_vm_states import HEADER, STATES, qualify_vm_states


class LuaVmStates(unittest.TestCase):
    def make(self, root):
        path = root / 'p9-lua-vm-states.csv'
        rows = []
        cumulative = [10, 10, 10, 10, 10, 10]
        callbacks = 60
        for index, epoch in enumerate((500, 3000, 7000), 1):
            deltas = [index] * 6
            cumulative = [a + b for a, b in zip(cumulative, deltas)]
            callbacks += sum(deltas)
            rows.append([index, epoch, 1, sum(deltas), *deltas, index, callbacks, *cumulative])
        # Ten independent CSV rows distinguish decimal producer_rows=10 from
        # unprefixed hexadecimal 10. Extra observations contain no new samples.
        for index in range(4, 11):
            rows.append([index, 7000 + index, 1, 0, *([0] * 6), index, callbacks, *cumulative])
        self.write_rows(path, rows)
        Path(str(path) + '.writer-status.txt').write_text(
            'normal_finish=1\noutput_ok=1\nrows_dropped=0\ninitialization_failed=0\n'
            'capacity_dropped=0\ncontention_dropped=0\noversized_dropped=0\n'
            'allocation_dropped=0\nio_dropped=0\nstopped_dropped=0\n')
        (root / 'openmw.log').write_text(
            'OptimizedMW Lua VM sampling: requested=1 api_available=1 interval_ms=1 '
            'acceptance_requires_callbacks=1 delivery_is_not_sample_time=1 stop_flushes_jit=1\n'
            'OptimizedMW Lua VM sampling: final_callbacks=96 native=16 interpreted=16 c=16 gc=16 jit=16 '
            'unknown=16 delivery_is_not_sample_time=1 stop_flushes_jit=1\n'
            'OptimizedMW VM work capture: producer_rows=10 callback_deliveries=96\n')
        return path, rows

    def write_rows(self, path, rows):
        with path.open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(HEADER)
            writer.writerows(rows)
            stream.write('# p9_capture_transport=bounded_mpsc_v1\n')

    def change_log(self, root, before, after):
        path = root / 'openmw.log'
        path.write_text(path.read_text().replace(before, after))

    def test_distribution_is_explicitly_not_exact_gc_or_spike_blame(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make(root)
            result = qualify_vm_states(root, {'start': 1000, 'movement': 5000, 'finish': 10000})
            self.assertTrue(result['qualified'])
            self.assertFalse(result['exact_per_spike_attribution'])
            self.assertFalse(result['exact_gc_elapsed_time'])
            self.assertEqual(result['callback_deliveries'], 36)
            self.assertEqual(result['outside_update_callbacks'], 60)
            self.assertEqual(result['delivered_states'], dict.fromkeys(STATES, 6))
            self.assertEqual(result['delivery_phase_distributions']['moving']['callback_deliveries'], 18)
            self.assertIn('delivery', result['phase_limit'])

    def test_start_attempt_cannot_qualify_zero_deliveries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make(root)
            self.change_log(root, 'final_callbacks=96', 'final_callbacks=0')
            self.change_log(root, 'callback_deliveries=96', 'callback_deliveries=0')
            with self.assertRaisesRegex(ValueError, 'No observed'):
                qualify_vm_states(root)

    def test_startup_only_samples_cannot_qualify_lua_updates(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path, rows = self.make(root)
            for row in rows:
                row[3:10] = [0] * 7
                row[11:] = [60, *([10] * 6)]
            self.write_rows(path, rows)
            with self.assertRaisesRegex(ValueError, 'during observed Lua updates'):
                qualify_vm_states(root)

    def test_each_transport_failure_is_rejected(self):
        for field in ('normal_finish', 'output_ok', 'rows_dropped', 'initialization_failed',
                      'capacity_dropped', 'contention_dropped', 'oversized_dropped',
                      'allocation_dropped', 'io_dropped', 'stopped_dropped'):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path, _ = self.make(root)
                status = Path(str(path) + '.writer-status.txt')
                current = '1' if field in ('normal_finish', 'output_ok') else '0'
                replacement = '0' if current == '1' else '1'
                status.write_text(status.read_text().replace(field + '=' + current, field + '=' + replacement))
                with self.assertRaisesRegex(ValueError, 'transport'):
                    qualify_vm_states(root)

    def test_tail_truncation_is_rejected_with_footer_present(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path, rows = self.make(root)
            self.write_rows(path, rows[:-1])
            with self.assertRaisesRegex(ValueError, 'tail-truncated'):
                qualify_vm_states(root)

    def test_missing_row_and_duplicate_row_are_rejected(self):
        for mode in ('missing', 'duplicate'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path, rows = self.make(root)
                rows = [rows[0], rows[2]] if mode == 'missing' else [rows[0], rows[0], rows[2]]
                self.write_rows(path, rows)
                with self.assertRaisesRegex(ValueError, 'sequence'):
                    qualify_vm_states(root)

    def test_missing_footer_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path, _ = self.make(root)
            path.write_text(path.read_text().replace('# p9_capture_transport=bounded_mpsc_v1', ''))
            with self.assertRaisesRegex(ValueError, 'footer'):
                qualify_vm_states(root)

    def test_missing_timing_limit_cannot_become_exact_attribution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make(root)
            self.change_log(root, 'delivery_is_not_sample_time=1', 'delivery_is_not_sample_time=0')
            with self.assertRaisesRegex(ValueError, 'caveat'):
                qualify_vm_states(root)

    def test_missing_completion_or_ambiguous_owner_is_rejected(self):
        for mode in ('missing', 'ambiguous'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self.make(root)
                path = root / 'openmw.log'
                lines = path.read_text().splitlines()
                lines = lines[:-1] if mode == 'missing' else [lines[0], *lines]
                path.write_text('\n'.join(lines))
                with self.assertRaisesRegex(ValueError, 'metadata'):
                    qualify_vm_states(root)

    def test_reset_negative_and_delta_overcount_are_rejected(self):
        for value in (0, -1, 999):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path, rows = self.make(root)
                if value == 999:
                    rows[1][4] = value
                else:
                    rows[1][12] = value
                self.write_rows(path, rows)
                with self.assertRaises(ValueError):
                    qualify_vm_states(root)

    def test_nan_repeated_frame_and_reversed_clock_are_rejected(self):
        for index, value in ((1, 'nan'), (0, 1), (1, 0)):
            with self.subTest(index=index, value=value), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path, rows = self.make(root)
                rows[1][index] = value
                self.write_rows(path, rows)
                with self.assertRaises(ValueError):
                    qualify_vm_states(root)

    def test_partial_and_unknown_schema_are_rejected(self):
        for mode in ('partial', 'header'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path, rows = self.make(root)
                if mode == 'partial':
                    rows[1] = rows[1][:-1]
                    self.write_rows(path, rows)
                else:
                    path.write_text(path.read_text().replace('gc_total', 'unproven_gc_ms'))
                with self.assertRaises(ValueError):
                    qualify_vm_states(root)

    def test_final_summary_mismatch_and_underflow_are_rejected(self):
        for before, after in (('callback_deliveries=96', 'callback_deliveries=95'),
                              ('gc=16', 'gc=15')):
            with self.subTest(before=before), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self.make(root)
                self.change_log(root, before, after)
                with self.assertRaises(ValueError):
                    qualify_vm_states(root)

    def test_invalid_route_window_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make(root)
            with self.assertRaisesRegex(ValueError, 'window'):
                qualify_vm_states(root, {'start': 100, 'finish': 90})

    def test_unprefixed_hex_final_totals_and_row_parity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); self.make(root)
            self.change_log(root, 'final_callbacks=96', 'final_callbacks=60')
            self.change_log(root, '=16', '=10')
            self.change_log(root, 'producer_rows=10 callback_deliveries=96', 'producer_rows=a callback_deliveries=60')
            result = qualify_vm_states(root)
            self.assertEqual(result['whole_vm_callbacks'], 96)
            self.assertEqual(result['whole_vm_states'], dict.fromkeys(STATES, 16))
            self.assertEqual(result['completion_numeric_encoding']['base'], 16)
            self.assertEqual(result['completion_numeric_encoding']['encoding'], 'unprefixed_hex')

    def test_digit_only_hex_row_count_is_independently_inferred(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); path, rows = self.make(root)
            for index in range(11, 17):
                rows.append([index, 7000 + index, 1, 0, *([0] * 6), index, 96, *([16] * 6)])
            self.write_rows(path, rows)
            self.change_log(root, 'final_callbacks=96', 'final_callbacks=60')
            self.change_log(root, '=16', '=10')
            self.change_log(root, 'callback_deliveries=96', 'callback_deliveries=60')
            result = qualify_vm_states(root)
            self.assertEqual(result['completion_numeric_encoding']['base'], 16)
            self.assertEqual(result['producer_rows'], 16)

    def test_differing_ambiguous_completion_totals_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); path, rows = self.make(root)
            self.write_rows(path, rows[:3])
            self.change_log(root, 'producer_rows=10', 'producer_rows=3')
            with self.assertRaisesRegex(ValueError, 'Ambiguous decimal/hex'):
                qualify_vm_states(root)

    def test_hex_completion_truncation_and_partial_tokens_are_rejected(self):
        for token in ('b', 'aZ', '0xa', 'a=1'):
            with self.subTest(token=token), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); self.make(root)
                self.change_log(root, 'producer_rows=10', 'producer_rows='+token)
                with self.assertRaises(ValueError):
                    qualify_vm_states(root)

    def test_duplicate_final_footer_or_numeric_field_are_rejected(self):
        for mode in ('final_line', 'producer_field', 'final_field'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); self.make(root); log = root/'openmw.log'
                if mode == 'final_line':
                    lines = log.read_text().splitlines(); log.write_text('\n'.join([*lines, lines[1]]))
                elif mode == 'producer_field':
                    self.change_log(root, 'producer_rows=10', 'producer_rows=10 producer_rows=10')
                else:
                    self.change_log(root, 'final_callbacks=96', 'final_callbacks=96 final_callbacks=96')
                with self.assertRaisesRegex(ValueError, 'ambiguous|Duplicate'):
                    qualify_vm_states(root)


if __name__ == '__main__':
    unittest.main()
