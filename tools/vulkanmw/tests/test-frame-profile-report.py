import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('profile_report', Path(__file__).parents[1] / 'frame-profile-report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class ReportTest(unittest.TestCase):
    def test_repeated_stages_sum_before_statistics(self):
        rows = [{'type': 'frame_end', 'frame': n, 'completed': '1', 'time_us': n * 1000} for n in (1, 2)]
        for n in (1, 2):
            rows += [dict(type='stage_end', frame=n, name='batch', ms='2')] * 6
        result = report.summarize(rows)
        self.assertEqual(result['stages_summed_per_frame_ms']['batch']['mean'], 12)

    def test_exclusive_accounting_and_delayed_gpu_frame(self):
        rows = [dict(type='frame_end', frame=1, completed='1', time_us=1000)]
        for name, inclusive, exclusive in [('frame', 10, 3), ('parent', 7, 5), ('child', 2, 2)]:
            rows.append(dict(type='profile_total', frame=1, name=name,
                             inclusive_ms=inclusive, exclusive_ms=exclusive, max_ms=inclusive, calls=1))
        rows.append(dict(type='profile_gpu', frame=99, source_frame='1', name='main', ms='4'))
        result = report.summarize(rows)
        self.assertFalse(result['health_issues'])
        self.assertEqual(sum(x['mean'] for x in result['cpu_exclusive_ms'].values()), 10)
        self.assertEqual(result['gpu_interval_ms']['main']['mean'], 4)

    def test_incomplete_accounting_is_excluded(self):
        rows = [dict(type='frame_end', frame=1, completed='1', time_us=1000),
                dict(type='profile_total', frame=1, name='frame', inclusive_ms=10,
                     exclusive_ms=3, max_ms=10, calls=1)]
        result = report.summarize(rows)
        self.assertEqual(result['profiled_frames'], 0)
        self.assertEqual(len(result['health_issues']), 1)

    def test_unhealthy_sample_is_excluded_even_when_root_balances(self):
        rows = [dict(type='frame_end', frame=1, completed='1', time_us=1000),
                dict(type='profile_total', frame=1, name='frame', inclusive_ms=10,
                     exclusive_ms=10, max_ms=10, calls=1),
                dict(type='profile_health', frame=1, dropped='1', unclosed='0')]
        result = report.summarize(rows)
        self.assertEqual(result['profiled_frames'], 0)
        self.assertEqual(len(result['health_issues']), 1)


if __name__ == '__main__':
    unittest.main()
