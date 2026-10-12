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

    def make_worker(self, root, totals, workers, flags=None, observations=None, epochs=None):
        windows = self.make(root, [value[0] for value in totals], flags)
        epochs = epochs or [i*1000 for i in range(len(totals))]
        observations = list(range(len(totals))) if observations is None else observations
        with (root/'v3-frame.csv').open('w', newline='') as stream:
            writer = csv.writer(stream); writer.writerow(('frame', 'epoch_ms'))
            writer.writerows(enumerate(epochs))
        with (root/'p9-lua-protected-work.csv').open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(('frame', 'epoch_ms', 'enabled', 'ordinary_calls', 'reused_calls', 'row_seq', *work.TOTALS))
            for seq, frame in enumerate(observations, 1):
                # Producer completion timestamps are separate from the main
                # frame clock used for elapsed-exposure joins.
                writer.writerow((frame, frame*1000, 1, *workers[frame], seq, *totals[frame]))
        (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows='+str(len(observations))+'\n')
        return windows

    def test_accepted_time_and_worker_inter_update_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reused = [0, 0, 0, 2, 4, 7, 10, 13, 999]
            ordinary = [0, 0, 0, 1, 3, 4, 6, 7, 8]
            totals = [(r, o+1, o) for r, o in zip(reused, ordinary)]
            workers = [(0, 0), (0, 0), (0, 0), (1, 1), (1, 2), (1, 1), (0, 2), (1, 1), (1, 1)]
            result = work.interval_work(root, self.make_worker(root, totals, workers))
            moving = result['phases']['moving']
            self.assertTrue(result['worker_decomposition_available'])
            self.assertEqual(moving['counts'], {'reused_total': 9, 'created_total': 4, 'ordinary_total': 4})
            self.assertEqual(moving['accepted_observation_ms'], 3000)
            self.assertEqual(moving['observation_pairs'], 3)
            self.assertEqual(moving['attempted_calls'], 13)
            self.assertEqual(moving['worker_attempted_calls'], 6)
            self.assertEqual(moving['inter_update_attempted_calls'], 7)
            self.assertEqual(moving['worker_ordinary_calls'], 2)
            self.assertEqual(moving['worker_reused_calls'], 4)
            self.assertEqual(moving['inter_update_ordinary_calls'], 2)
            self.assertEqual(moving['inter_update_reused_calls'], 5)
            self.assertAlmostEqual(moving['attempted_per_observed_second'], 13/3)
            self.assertEqual(moving['worker_attempted_per_observed_second'], 2)
            self.assertAlmostEqual(moving['inter_update_attempted_per_observed_second'], 7/3)
            self.assertEqual(moving['worker_attempted_per_observation'], 2)
            fixed = result['phases']['fixed_view']
            self.assertEqual(fixed['attempted_calls'], 3)
            self.assertEqual(fixed['accepted_observation_ms'], 1000)
            self.assertEqual(fixed['worker_attempted_calls'], 2)
            self.assertEqual(fixed['inter_update_attempted_calls'], 1)
            self.assertIn('not successful Lua callbacks', result['attempt_semantics'])

    def test_legacy_header_preserves_counts_without_inventing_worker_split(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = work.interval_work(root, self.make(root, [0, 0, 0, 2, 4, 7, 10, 13, 999]))
            moving = result['phases']['moving']
            self.assertFalse(result['worker_decomposition_available'])
            self.assertEqual(result['schema'], 1)
            self.assertEqual(moving['attempted_calls'], 9)
            self.assertEqual(moving['accepted_observation_ms'], 3000)
            self.assertEqual(moving['attempted_per_observed_second'], 3)
            for key in ('worker_attempted_calls', 'inter_update_attempted_calls',
                        'worker_attempted_per_observed_second', 'inter_update_attempted_per_observed_second',
                        'worker_attempted_per_observation'):
                self.assertIsNone(moving[key])

    def test_negative_worker_residual_rejects_even_excluded_loading_rows(self):
        for flags in (None, {5: 1}):
            for bad_worker in ((2, 0), (0, 2), (-1, 0)):
                with self.subTest(flags=flags, worker=bad_worker), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    totals = [(i, i+1, i) for i in range(9)]
                    workers = [(0, 0)] + [(1, 1)]*8
                    workers[5] = bad_worker
                    windows = self.make_worker(root, totals, workers, flags)
                    with self.assertRaisesRegex(ValueError, 'worker observation|inter-update decomposition'):
                        work.interval_work(root, windows)

    def test_partial_worker_header_rejects(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, list(range(9)))
            path = root/'p9-lua-protected-work.csv'
            lines = path.read_text().splitlines()
            lines[0] += ',ordinary_calls'
            lines[1:] = [line+',0' for line in lines[1:]]
            path.write_text('\n'.join(lines)+'\n')
            with self.assertRaisesRegex(ValueError, 'worker header'):
                work.interval_work(root, windows)

    def test_loading_and_phase_boundary_pairs_do_not_add_time_or_attempts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            totals = [(i*10, i+1, i) for i in range(9)]
            workers = [(0, 0)] + [(1, 2)]*8
            result = work.interval_work(root, self.make_worker(root, totals, workers, {5: 1}))
            fixed, moving = (result['phases'][name] for name in ('fixed_view', 'moving'))
            self.assertEqual(fixed['accepted_observation_ms'], 1000)
            self.assertEqual(fixed['attempted_calls'], 11)
            self.assertEqual(moving['accepted_observation_ms'], 1000)
            self.assertEqual(moving['attempted_calls'], 11)
            self.assertEqual(moving['worker_attempted_calls'], 3)
            self.assertEqual(moving['inter_update_attempted_calls'], 8)
            # 3->4 crosses the phase boundary; 4->5 and 5->6 touch loading.
            self.assertEqual(moving['observation_pairs'], 1)

    def test_sparse_pair_cannot_cross_an_interior_loading_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            totals = [(i*10, i+1, i) for i in range(9)]
            workers = [(0, 0)] + [(1, 2)]*8
            result = work.interval_work(root, self.make_worker(root, totals, workers, {5: 1}, [0, 2, 3, 4, 6, 7, 8]))
            moving = result['phases']['moving']
            self.assertEqual(moving['accepted_observation_ms'], 1000)
            self.assertEqual(moving['attempted_calls'], 11)
            self.assertEqual(moving['maximum_observation_gap_ms'], 1000)

    def test_same_frame_or_tied_clock_never_creates_a_time_denominator(self):
        for same_frame in (False, True):
            with self.subTest(same_frame=same_frame), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                totals = [(i*10, i+1, i) for i in range(9)]
                workers = [(0, 0)] + [(1, 2)]*8
                epochs = [i*1000 for i in range(9)]; epochs[6] = epochs[5]
                windows = self.make_worker(root, totals, workers, epochs=epochs)
                if same_frame:
                    path = root/'p9-lua-protected-work.csv'
                    with path.open(newline='') as stream:
                        rows = list(csv.reader(stream))
                    rows[7][0] = '5'
                    with path.open('w', newline='') as stream:
                        csv.writer(stream).writerows(rows)
                moving = work.interval_work(root, windows)['phases']['moving']
                self.assertEqual(moving['accepted_observation_ms'], 3000)
                self.assertEqual(moving['observation_pairs'], 2)
                self.assertEqual(moving['attempted_calls'], 22)
                self.assertAlmostEqual(moving['attempted_per_observed_second'], 22/3)

    def test_invalid_clocks_reject_instead_of_guessing_observation_time(self):
        for clock_kind in ('main_nan', 'main_reverse', 'route_nan', 'route_reverse', 'movement_outside'):
            with self.subTest(clock=clock_kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                totals = [(i, i+1, i) for i in range(9)]
                workers = [(0, 0)] + [(1, 1)]*8
                epochs = [i*1000 for i in range(9)]
                if clock_kind == 'main_nan': epochs[5] = float('nan')
                if clock_kind == 'main_reverse': epochs[5] = 2500
                windows = self.make_worker(root, totals, workers, epochs=epochs)
                if clock_kind == 'route_nan': windows['start'] = float('nan')
                if clock_kind == 'route_reverse': windows['finish'] = 0
                if clock_kind == 'movement_outside': windows['movement'] = 10000
                with self.assertRaisesRegex(ValueError, 'clock'):
                    work.interval_work(root, windows)

    def test_empty_phase_rates_are_unavailable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            totals = [(i, i+1, i) for i in range(4)]
            workers = [(0, 0)] + [(1, 1)]*3
            moving = work.interval_work(root, self.make_worker(root, totals, workers))['phases']['moving']
            self.assertEqual(moving['accepted_observation_ms'], 0)
            self.assertEqual(moving['attempted_calls'], 0)
            for key in ('attempted_per_observed_second', 'worker_attempted_per_observed_second',
                        'inter_update_attempted_per_observed_second', 'worker_attempted_per_observation'):
                self.assertIsNone(moving[key])

    def test_missing_endpoint_clock_excludes_pair_without_using_producer_epoch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            totals = [(i, i+1, i) for i in range(9)]
            workers = [(0, 0)] + [(1, 1)]*8
            windows = self.make_worker(root, totals, workers)
            path = root/'v3-frame.csv'
            with path.open(newline='') as stream:
                rows = list(csv.reader(stream))
            with path.open('w', newline='') as stream:
                csv.writer(stream).writerows([row for row in rows if row[0] != '5'])
            moving = work.interval_work(root, windows)['phases']['moving']
            self.assertEqual(moving['accepted_observation_ms'], 1000)
            self.assertEqual(moving['observation_pairs'], 1)
            self.assertEqual(moving['attempted_calls'], 2)
            self.assertEqual(moving['attempted_per_observed_second'], 2)

    def test_initial_worker_cannot_exceed_lifetime_counter(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            totals = [(i, i+1, i) for i in range(9)]
            workers = [(1, 0)] + [(1, 1)]*8
            windows = self.make_worker(root, totals, workers)
            with self.assertRaisesRegex(ValueError, 'inter-update decomposition'):
                work.interval_work(root, windows)

    def test_positive_combined_residual_does_not_hide_negative_kind(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            totals = [(i*10, i+1, i) for i in range(9)]
            workers = [(0, 0)] + [(1, 2)]*8
            workers[5] = (2, 0)
            windows = self.make_worker(root, totals, workers)
            with self.assertRaisesRegex(ValueError, 'inter-update decomposition'):
                work.interval_work(root, windows)

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

    def test_hex_letters_footer_is_constrained_to_exact_csv_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, list(range(26)))
            (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows=1a\n')
            result = work.interval_work(root, windows)
            self.assertEqual(result['producer_footer']['base'], 16)
            self.assertEqual(result['producer_footer']['parsed_rows'], 26)
            self.assertEqual(result['producer_footer']['encoding'], 'unprefixed_hex')

    def test_digit_only_footer_base_is_decided_by_csv_parity(self):
        for length, base in ((10, 10), (16, 16)):
            with self.subTest(length=length), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); windows = self.make(root, list(range(length)))
                (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows=10\n')
                self.assertEqual(work.interval_work(root, windows)['producer_footer']['base'], base)

    def test_equivalent_single_digit_base_is_reported_honestly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); windows = self.make(root, list(range(9)))
            receipt = work.interval_work(root, windows)['producer_footer']
            self.assertIsNone(receipt['base'])
            self.assertEqual(receipt['encoding'], 'decimal_or_hex_equivalent')

    def test_hex_tail_truncation_and_partial_token_are_rejected(self):
        for token in ('1a', '19acZ', '0x19ac', '19ac=1'):
            with self.subTest(token=token), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); windows = self.make(root, list(range(25)))
                (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows='+token+'\n')
                with self.assertRaises(ValueError):
                    work.interval_work(root, windows)

    def test_duplicate_footer_and_duplicate_row_field_are_rejected(self):
        for suffix in ('\nOptimizedMW protected work capture: producer_rows=4', ' producer_rows=4'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); windows = self.make(root, list(range(4)))
                (root/'openmw.log').write_text('OptimizedMW protected work capture: producer_rows=4'+suffix+'\n')
                with self.assertRaisesRegex(ValueError, 'ambiguous|token'):
                    work.interval_work(root, windows)


if __name__ == '__main__':
    unittest.main()
