import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('runtime_diagnostics', Path(__file__).with_name('runtime-diagnostics.py'))
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)


class RuntimeReportTests(unittest.TestCase):
    def analyze(self, rows, tail=''):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'runtime.jsonl'
            path.write_text(''.join(json.dumps(dict(schema=2, time_us=i*1000000, **row))+'\n' for i, row in enumerate(rows))+tail, encoding='utf-8')
            return diag.analyze(path)

    def test_pressure_trim_counts_are_not_reclaimed_byte_claims(self):
        result = self.analyze([dict(type='host_memory_trim', owner='owners', removed_entries=7),
                               dict(type='host_memory_trim', owner='owners', removed_entries=3)])
        self.assertEqual(result['counts']['cache_entries_trimmed'], 10)
        self.assertTrue(any('not a count of unique assets or bytes' in s for s in result['findings']))

    def test_pressure_probe_validity_remains_separate(self):
        result = self.analyze([dict(type='host_memory_trim', owner='owners', removed_entries=1,
            physical_valid=0, physical_available_bytes=0, process_valid=0, private_commit_bytes=0,
            commit_valid=0, commit_available_bytes=0)])
        entry = next(e for e in result['series'] if e['type'] == 'host_memory_trim')
        self.assertNotIn('physical_available_bytes', entry['peak'])
        self.assertNotIn('private_commit_bytes', entry['peak'])
        self.assertNotIn('commit_available_bytes', entry['peak'])

    def test_missing_and_empty_are_not_passes(self):
        result = self.analyze([])
        self.assertTrue(any('CAPTURE FAILURE' in f for f in result['findings']))
        with tempfile.TemporaryDirectory() as d:
            result = diag.report(d)
            self.assertIn('missing', result['findings'][0])
            self.assertTrue((Path(d)/'memory-report.md').exists())

    def test_memory_layers_never_added(self):
        result = self.analyze([
            dict(type='os_memory', process_valid=1, physical_valid=1, private_commit_bytes=200, working_set_bytes=100, physical_available_bytes=30),
            dict(type='os_memory', process_valid=1, physical_valid=1, private_commit_bytes=300, working_set_bytes=90, physical_available_bytes=20),
            dict(type='vsg_pool', owner='pool', reserved_bytes=40), dict(type='recorder_end')])
        process = result['series'][0]
        self.assertEqual(process['peak']['working_set_bytes'], 100)
        self.assertEqual(process['delta']['private_commit_bytes'], 100)
        self.assertEqual(process['delta']['working_set_bytes'], -10)
        self.assertNotIn('total_memory', result)
        self.assertTrue(any('Do not add' in f for f in result['findings']))

    def test_unavailable_os_values_not_zero_measurements(self):
        result = self.analyze([dict(type='os_memory', process_valid=0, working_set_bytes=0), dict(type='recorder_end')])
        self.assertNotIn('working_set_bytes', result['series'][0]['peak'])

    def test_unavailable_gpu_budget_not_zero_measurement(self):
        result = self.analyze([dict(type='vulkan_heap', budget_available=0, heap_size_bytes=100, usage_bytes=0)])
        self.assertNotIn('usage_bytes', result['series'][0]['last'])
        self.assertEqual(result['series'][0]['last']['heap_size_bytes'], 100)

    def test_cache_coverage_and_interval_hits(self):
        result = self.analyze([dict(type='cache', owner='images', instance=1, lookups=10, hits=8, limited=0),
                               dict(type='cache', owner='images', instance=1, lookups=20, hits=13, limited=1)])
        self.assertEqual(result['series'][0]['interval_hit_ratio'], .5)
        self.assertTrue(any('PARTIAL PAYLOAD' in f for f in result['findings']))

    def test_distinct_cache_instances_do_not_merge(self):
        result = self.analyze([dict(type='cache', owner='images', instance=1, entries=2),
                               dict(type='cache', owner='images', instance=2, entries=3)])
        self.assertEqual(len(result['series']), 2)

    def test_nested_work_is_inclusive_and_unfinished_is_not_crash(self):
        result = self.analyze([dict(type='work_begin', operation=1, owner='outer'),
                               dict(type='work_begin', operation=2, owner='inner'),
                               dict(type='work_end', operation=2, owner='inner', elapsed_us=30)])
        self.assertEqual(len(result['unfinished_work']), 1)
        self.assertEqual(result['longest_completed_work'][0]['elapsed_us'], 30)
        self.assertFalse(any(f.startswith('RUN FAILURE') for f in result['findings']))

    def test_record_loss_and_partial_tail_explicit(self):
        result = self.analyze([dict(type='recorder_stats', dropped=15), dict(type='capture_limit')], '{"schema":')
        self.assertEqual(result['counts']['malformed_rows'], 1)
        self.assertTrue(any('15 events' in f for f in result['findings']))
        self.assertTrue(any('no recorder shutdown' in f for f in result['findings']))

    def test_duplicate_shared_payload_count_not_added(self):
        result = self.analyze([dict(type='cache', owner='images', known_payload_bytes=256, shared_payload_refs=7)])
        self.assertEqual(result['series'][0]['last']['known_payload_bytes'], 256)

    def test_pending_retirement_is_not_called_a_leak(self):
        result = self.analyze([dict(type='retirement', owner='dynamic', pending_roots=5, completed_roots_pending_collection=4)])
        self.assertTrue(any('can be normal' in f for f in result['findings']))
        self.assertFalse(any(f.startswith('LEAK') for f in result['findings']))

    def test_ui_alias_is_observed_not_pixel_success(self):
        result = self.analyze([dict(type='ui_missing_alias', owner='mygui', identity='preview')])
        self.assertTrue(any('unresolved native texture' in f for f in result['findings']))
        self.assertFalse(any('pixel pass' in f.lower() for f in result['findings']))

    def test_untrusted_oversized_line_does_not_prevent_later_records(self):
        result = self.analyze([], 'x' * (diag.MAX_LINE+500) + '\n' + json.dumps(dict(schema=2, type='recorder_end')) + '\n')
        self.assertEqual(result['counts']['oversized_rows'], 1)
        self.assertEqual(result['counts']['recorder_end'], 1)

    def test_examples_and_history_are_bounded(self):
        result = self.analyze([dict(type='terrain_job', job=i) for i in range(1000)])
        self.assertEqual(len(result['bounded_examples']), 32)
        self.assertEqual(result['counts']['terrain_job'], 1000)

    def test_failed_process_cannot_be_promoted_from_clean_telemetry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'runtime.jsonl').write_text('{"schema":2,"type":"recorder_end"}\n')
            (root/'manifest.json').write_text(json.dumps(dict(state='exited',exit_code=1)))
            result = diag.report(directory)
            self.assertTrue(result['findings'][0].startswith('RUN FAILURE'))

    def test_writer_output_from_native_modes(self):
        if not CAPTURE_DIRECTORY:
            self.skipTest('Native capture directory not supplied')
        root = Path(CAPTURE_DIRECTORY)
        self.assertFalse((root/'off.jsonl').exists())
        for mode in ('standard', 'focused'):
            rows = [json.loads(line) for line in (root/f'{mode}.jsonl').read_text().splitlines()]
            end = rows[-1]
            self.assertEqual(end['type'], 'recorder_end')
            self.assertEqual(end['attempted'], end['written'] + end['dropped'])
            self.assertGreater(end['queue_bytes'], 0)
            self.assertLess(end['queue_bytes'], 8*1024*1024)
            self.assertTrue(any(r.get('type') == 'thread_fixture' for r in rows))
            self.assertEqual(len({(r['thread'], r['sequence']) for r in rows if r['type']=='thread_fixture'}),
                             sum(r['type']=='thread_fixture' for r in rows))
            result = diag.analyze(root/f'{mode}.jsonl')
            self.assertEqual(result['counts'].get('malformed_rows', 0), 0)


CAPTURE_DIRECTORY = None
if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--capture-directory')
    args, remaining = parser.parse_known_args()
    CAPTURE_DIRECTORY = args.capture_directory
    unittest.main(argv=['test-runtime-diagnostics.py', *remaining])
