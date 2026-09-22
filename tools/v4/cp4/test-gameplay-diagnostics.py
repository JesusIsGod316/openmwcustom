import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('diagnostics', Path(__file__).with_name('gameplay-diagnostics.py'))
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class ReportTests(unittest.TestCase):
    def test_allocation_and_terrain_observations_do_not_invent_performance_results(self):
        result = self.analyze([
            dict(type='dynamic_allocation', phase='before', summary='images=96 unique_image_payloads=1'),
            dict(type='dynamic_allocation', phase='failed', summary='pending_image_payload_bytes=100'),
            dict(type='terrain_preload_work', queue_ms='8.5', work_ms='1200', views='1', aborted='0')])
        self.assertEqual(len(result['allocation_samples']), 2)
        self.assertEqual(result['terrain_preload_work'][0]['queue_ms'], 8.5)
        self.assertFalse(result['terrain_preload_work'][0]['aborted'])
        self.assertEqual(result['distinct_osg_frame_stamps'], 0)
        self.assertFalse(any('RUNTIME FAILURE' in s for s in result['findings']))

    def test_repeated_effect_churn_is_investigation_not_failure(self):
        row = dict(type='residency', actors_reused='4', actors_rebuilt='0', effects_reused='450', effects_rebuilt='100')
        result = self.analyze([row] * 3)
        self.assertTrue(any('INVESTIGATE effect resource churn' in s for s in result['findings']))
        self.assertFalse(any('RUNTIME FAILURE' in s for s in result['findings']))
        self.assertFalse(any('resource churn' in s for s in self.analyze([row])['findings']))

    def test_cell_insertion_breakdown_keeps_identity(self):
        result = self.analyze([dict(type='cell_insertion', cell='Seyda Neen', refs='20',
                                    canonical_render_ms='100', physics_ms='5', v4_publication_ms='7', progress_ms='46')])
        self.assertEqual(result['cell_insertions'][0]['cell'], 'Seyda Neen')
        self.assertEqual(result['inclusive_stage_timings']['loading:insertion:physics']['median_ms'], 5)
        self.assertEqual(result['inclusive_stage_timings']['loading:insertion:progress']['median_ms'], 46)

    def analyze(self, rows, tail=''):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'gameplay.jsonl'
            path.write_text(''.join(json.dumps(dict(schema=1, frame=i, **row)) + '\n'
                                    for i, row in enumerate(rows)) + tail, encoding='utf-8')
            return diagnostics.analyze(path)

    def test_multiple_findings_in_one_capture(self):
        result = self.analyze([
            dict(type='frame_end', skeleton_skipped_cull='2', camera_callbacks='0'),
            dict(type='osg_update', viewer_done='1'),
            dict(type='pick', view_delta='55'),
            dict(type='actor_geometry', actor='1:1:1', placement_hash='p', sample_hash='g', stream_mismatches='3', nonfinite='1'),
            dict(type='submission', submit='-4', present='0')])
        self.assertEqual(result['counts']['skipped_cull'], 2)
        self.assertEqual(result['counts']['stream_mismatches'], 3)
        self.assertEqual(result['counts']['submission_failures'], 1)
        self.assertGreaterEqual(len(result['findings']), 5)

    def test_stationary_pose_not_automatically_failure(self):
        rows = [dict(type='actor_pose', actor='1', local_hash='a')]
        rows += [dict(type='actor_geometry', actor='1', placement_hash='b', sample_hash='c', stream_mismatches='0', nonfinite='0')] * 3
        self.assertFalse(any('INVESTIGATE' in text for text in self.analyze(rows)['findings']))

    def test_moving_with_static_pose_flagged_as_hypothesis(self):
        rows = [dict(type='actor_pose', actor='1', local_hash='a')]
        rows += [dict(type='actor_geometry', actor='1', placement_hash=str(i), sample_hash='c', stream_mismatches='0', nonfinite='0') for i in range(3)]
        self.assertTrue(any('INVESTIGATE' in text for text in self.analyze(rows)['findings']))

    def test_native_crash_partial_line_retains_stage(self):
        result = self.analyze([dict(type='stage_begin', name='submit_present')], '{"schema":')
        self.assertEqual(result['counts']['malformed_rows'], 1)
        self.assertTrue(any('submit_present' in text for text in result['findings']))

    def test_missing_trace_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            result = diagnostics.report(directory)
            self.assertIn('CAPTURE FAILURE', result['findings'][0])
            self.assertTrue((Path(directory) / 'report.md').exists())

    def test_actor_generations_not_combined(self):
        rows = [dict(type='actor_pose', actor='1:1:1', local_hash='a')]
        rows += [dict(type='actor_geometry', actor='1:1:2', placement_hash=str(i), sample_hash='c', stream_mismatches='0', nonfinite='0') for i in range(3)]
        self.assertFalse(any('INVESTIGATE' in text for text in self.analyze(rows)['findings']))

    def test_capture_limit_not_a_pass(self):
        self.assertTrue(any('limit' in text for text in self.analyze([dict(type='capture_limit')])['findings']))

    def test_loading_outside_sampled_frames_and_nested_ids(self):
        result = self.analyze([
            dict(type='operation_begin', id='1', name='load_cell', identity='outer'),
            dict(type='operation_begin', id='2', name='load_cell', identity='inner'),
            dict(type='operation_end', id='2', name='load_cell', identity='inner', ms='5', unwinding='0'),
            dict(type='failure', message="NPC test bone='missing'"),
            dict(type='operation_end', id='1', name='load_cell', identity='outer', ms='50', unwinding='1')])
        self.assertEqual(result['sampled_frames'], 0)
        self.assertEqual(result['distinct_osg_frame_stamps'], 0)
        self.assertEqual(result['inclusive_stage_timings']['loading:load_cell']['samples'], 2)
        self.assertEqual(sum('RUNTIME FAILURE' in s for s in result['findings']), 2)
        self.assertFalse(any('INCOMPLETE LOADING' in s for s in result['findings']))

    def test_loading_interrupted_not_misclassified_as_crash(self):
        result = self.analyze([dict(type='operation_begin', id='1', name='load_cell', identity='Seyda Neen')])
        self.assertTrue(any('INCOMPLETE LOADING' in s and 'Seyda Neen' in s for s in result['findings']))
        self.assertFalse(any('RUNTIME FAILURE' in s for s in result['findings']))

    def test_fatal_log_and_exit_override_clean_frame_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'gameplay.jsonl').write_text('', encoding='utf-8')
            (root / 'openmw.log').write_text('[0 I] Loading cell Seyda Neen\n[1 E] Fatal error: attachment failed\n[2 I] Quitting peacefully\n', encoding='utf-8')
            (root / 'manifest.json').write_text(json.dumps(dict(state='exited', exit_code=1)), encoding='utf-8')
            result = diagnostics.report(directory)
            self.assertTrue(any('RUNTIME FAILURE' in s and 'attachment failed' in s for s in result['findings']))
            self.assertTrue(any('RUN FAILED' in s for s in result['findings']))
            self.assertTrue(any('independent loading' in s for s in result['findings']))

    def test_running_manifest_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'manifest.json').write_text(json.dumps(dict(state='running')), encoding='utf-8')
            self.assertTrue(any('RUN INCOMPLETE' in s for s in diagnostics.report(directory)['findings']))

    def test_manual_launch_preserves_originals_and_space_arguments(self):
        with tempfile.TemporaryDirectory(prefix='cp4 test ') as directory:
            base = Path(directory)
            exe = base / 'openmw.exe'
            exe.write_bytes(b'fixture')
            shader_dir = base / 'resources/shaders'
            shader_dir.mkdir(parents=True)
            (shader_dir / 'fixture.vert').write_text('void main() {}')
            (shader_dir / 'shader-package.json').write_text(json.dumps({
                'schema': 1, 'overlay_sha256': '6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255',
                'files': {'fixture.vert': diagnostics.sha256(shader_dir / 'fixture.vert')}}))
            user = base / 'normal config'
            user.mkdir()
            (base / 'openmw.cfg').write_text('config="?userconfig?"\n')
            originals = {'settings.cfg': b'[Video]\nrenderer backend=opengl\n',
                         'openmw.cfg': b'content=Morrowind.esm\n', 'openmw-crash.dmp': b'original'}
            for name, content in originals.items():
                (user / name).write_bytes(content)
            args = SimpleNamespace(executable=str(exe), user_config=str(user), evidence_root=str(base / 'evidence'),
                                   dll_directory=[], osg_library_path=None, source_head='fixture', source_diff_sha256='fixture')
            class Process:
                pid = 123
                def wait(self): return 0
            with patch.object(diagnostics.subprocess, 'check_output', return_value=b''), \
                 patch.object(diagnostics.subprocess, 'Popen', return_value=Process()) as start:
                diagnostics.launch(args)
            command = start.call_args.args[0]
            self.assertEqual(command[0], str(exe))
            self.assertEqual(len(command), 14)
            self.assertEqual(command[1], '--replace=config')
            self.assertEqual(command[2:5], ['--config', str(user), '--config'])
            self.assertEqual(command.count(str(user)), 1)
            self.assertEqual(command[6], '--user-data')
            self.assertEqual(command[7], str(Path(command[5]) / 'user-data'))
            self.assertEqual(command[8:10], ['--resources', str(base / 'resources')])
            self.assertNotIn('--load-savegame', command)
            self.assertEqual(command[10:], ['--skip-menu=false', '--new-game=false', '--script-run', ''])
            self.assertEqual(start.call_args.kwargs['env']['OPENMW_RUNTIME_DIAGNOSTICS'], 'standard')
            self.assertTrue((Path(command[5]) / 'user-data').is_dir())
            self.assertIn('user-data=', (Path(command[5]) / 'openmw.cfg').read_text())
            self.assertTrue(Path(command[5]).with_suffix('.zip').is_file())
            for name, content in originals.items():
                self.assertEqual((user / name).read_bytes(), content)
            manifest = json.loads((Path(command[5]) / 'manifest.json').read_text())
            self.assertEqual(manifest['state'], 'exited')
            self.assertEqual(manifest['diagnostics'], 'standard')
            self.assertFalse(manifest['regular_saves_copied'])
            self.assertTrue(manifest['original_config_unchanged']['settings.cfg'])
            self.assertTrue(all(manifest['original_chain_unchanged'].values()))
            private_settings = (Path(command[5]) / 'settings.cfg').read_text()
            self.assertIn('renderer backend = vulkan', private_settings)
            self.assertNotIn('[V3]', private_settings)
            self.assertFalse((Path(command[5]) / 'user-data/saves').exists())
            import zipfile
            with zipfile.ZipFile(Path(command[5]).with_suffix('.zip')) as bundle:
                self.assertTrue(set(bundle.namelist()).issubset({
                    'manifest.json', 'console.log', 'openmw.log', 'gameplay.jsonl', 'runtime.jsonl',
                    'report.md', 'report.json', 'memory-report.md', 'memory-report.json'}))
                self.assertIn('memory-report.json', bundle.namelist())

    def test_packaged_helper_does_not_inspect_parent_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertIsNone(diagnostics.source_checkout(root / 'gameplay-diagnostics.py'))
            self.assertIsNone(diagnostics.source_checkout(Path('/gameplay-diagnostics.py')))
            source_file = root / 'tools/v4/cp4/gameplay-diagnostics.py'
            source_file.parent.mkdir(parents=True)
            source_file.touch()
            self.assertIsNone(diagnostics.source_checkout(source_file))
            (root / '.git').mkdir()
            self.assertEqual(diagnostics.source_checkout(source_file), root.resolve())

    def test_launch_refuses_missing_shader_package_before_starting(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'openmw.exe'
            exe.write_bytes(b'fixture')
            args = SimpleNamespace(executable=str(exe))
            with patch.object(diagnostics.subprocess, 'Popen') as start:
                with self.assertRaisesRegex(ValueError, 'Missing shader package manifest'):
                    diagnostics.launch(args)
                start.assert_not_called()


if __name__ == '__main__':
    unittest.main()
