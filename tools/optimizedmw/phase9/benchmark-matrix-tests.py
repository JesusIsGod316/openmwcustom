"""Clock, context and pacing checks independent of game timing noise."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).parent
spec=importlib.util.spec_from_file_location('matrix',ROOT/'benchmark_matrix.py')
matrix=importlib.util.module_from_spec(spec);spec.loader.exec_module(matrix)


class Benchmarks(unittest.TestCase):
    def test_private_timeout_without_run_receipt_keeps_actual_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path/'Desktop').mkdir()
            (path/'Desktop'/'DESKTOP-RECEIPT.json').write_text(json.dumps({
                'timed_out':True, 'exit_code':124, 'elapsed_seconds':600.6375}))
            with self.assertRaisesRegex(ValueError, 'timed out after 600.6375 seconds'):
                matrix.completed_run_receipts(path, 124)
            # A partial receipt must never override a timeout or become timing evidence.
            (path/'RUN.json').write_text(json.dumps({'exit_code':0, 'launch_failure':None}))
            with self.assertRaisesRegex(ValueError, 'exists but is not qualified'):
                matrix.completed_run_receipts(path, 124)

    def test_private_failure_and_missing_success_receipts_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path/'Desktop').mkdir()
            desktop = path/'Desktop'/'DESKTOP-RECEIPT.json'
            desktop.write_text(json.dumps({'timed_out':False, 'exit_code':2}))
            with self.assertRaisesRegex(ValueError, 'Private launcher failed with exit code 2'):
                matrix.completed_run_receipts(path, 2)
            desktop.write_text(json.dumps({'timed_out':False, 'exit_code':0}))
            with self.assertRaisesRegex(ValueError, 'without an automatic run receipt'):
                matrix.completed_run_receipts(path, 0)
            receipt = path/'RUN.json'
            receipt.write_text(json.dumps({'exit_code':1, 'launch_failure':'route failed'}))
            with self.assertRaisesRegex(ValueError, 'game exit code 1; launcher error route failed'):
                matrix.completed_run_receipts(path, 0)
            receipt.write_text(json.dumps({'exit_code':0, 'launch_failure':None}))
            returned, _ = matrix.completed_run_receipts(path, 0)
            self.assertEqual(returned['exit_code'], 0)

    def test_matrix_preserves_timeout_case_without_starting_game(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root/'game'; game.mkdir()
            for name in ('openmw.exe', 'BUILD-IDENTITY.json', 'symbols.pdb'):
                (game/name).write_bytes(b'offline test input; never executed')
            catalog = root/'catalog.json'
            catalog.write_text(json.dumps({'routes':[{'id':'Balmora', 'inputs':[],
                'pinned_acquisition':str(root/'unused_route')}]}))
            plan = root/'plan.json'
            plan.write_text(json.dumps({'game_directory':str(game), 'route_catalog':str(catalog),
                'symbols':str(game/'symbols.pdb'), 'powershell':'not-executed',
                'cases':[{'id':'TimeoutCase', 'route':'Balmora'}]}))
            output = root/'output'

            def timeout_launcher(executable, arguments, working, desktop_path, deadline):
                self.assertEqual(executable, 'not-executed')
                desktop_path.mkdir()
                matrix.benchmark_desktop.save(desktop_path/'DESKTOP-RECEIPT.json', {
                    'timed_out':True, 'exit_code':124, 'elapsed_seconds':600.6375})
                return 124

            with patch.object(matrix.benchmark_desktop, 'launch', side_effect=timeout_launcher) as launch:
                with self.assertRaisesRegex(ValueError, 'Private launcher timed out'):
                    matrix.run_matrix(plan, output)
                launch.assert_called_once()
            state = matrix.read_json(output/'MATRIX.json')
            self.assertEqual(state['state'], 'failed')
            self.assertEqual(state['runs'], [])
            failure = matrix.read_json(output/'01_TimeoutCase'/'FAILED-RUN.json')
            self.assertEqual(failure, state['failed_run'])
            self.assertEqual(failure['state'], 'FAILED_NOT_QUALIFIED')
            self.assertFalse(failure['receipt_exists'])
            self.assertTrue(failure['desktop_receipt_exists'])
            self.assertNotIn('No such file', failure['error'])

    def test_low_ms_jitter_and_local_spikes(self):
        stable=matrix.pacing([5.]*100)
        jitter=matrix.pacing([4.,6.]*50)
        self.assertEqual(jitter['over_16_67'],0)
        self.assertGreater(jitter['adjacent_change_p95_ms'],stable['adjacent_change_p95_ms'])
        self.assertGreater(jitter['mad_ms'],stable['mad_ms'])
        spike=matrix.pacing([5.]*50+[9.]+[5.]*49)
        self.assertEqual(spike['relative_spikes'],1)
        self.assertEqual(spike['relative_excess_ms'],4)

    def test_pins_detect_same_length_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'input';path.write_bytes(b'ab')
            pins=[matrix.identity(path)];matrix.verify(pins)
            path.write_bytes(b'cd')
            with self.assertRaises(ValueError):matrix.verify(pins)

    def test_wall_clock_midnight_and_context_exclusion(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)
            # Derive the launch UTC from the local log date; both clocks are
            # separate from Lua's boot-relative timer.
            from datetime import datetime
            launch=datetime(2026,10,10,23,59,55).astimezone().isoformat()
            (path/'RUN-MANIFEST.json').write_text(json.dumps({'process':{'launch_utc':launch}}))
            rows=[('23:59:58.000',{'event':'start'}),
                  ('00:00:02.000',{'event':'check','guidance_command':{'nominal_movement':1}}),
                  ('00:00:06.000',{'event':'finish'})]
            (path/'playback-stdout.log').write_text('\n'.join(f'[{t} I] OMWPLAY|'+json.dumps(r) for t,r in rows))
            clock=matrix.route_windows(path,launch)
            self.assertEqual(clock['finish']-clock['start'],8000)
            (path/'benchmark-context.csv').write_text('frame,viewer_end_frame,begin_flags,end_flags,observed_flags\n1,1,0,0,0\n2,2,0,1,1\n3,3,0,0,0\n')
            (path/'v3-frame.csv').write_text('frame,epoch_ms,wall_ms\n'+
                f'1,{clock["start"]+2000},5\n2,{clock["start"]+3000},100\n3,{clock["finish"]-2000},6\n')
            result=matrix.analyze(path)
            self.assertEqual(result['excluded_context_frames'],1)
            self.assertEqual(result['metrics']['route']['frames'],2)
            self.assertEqual(result['metrics']['route']['maximum_ms'],6)
            self.assertEqual(result['metrics']['route_all_frames']['maximum_ms'],100)
            self.assertEqual(result['metrics']['fixed_view']['frames'],1)
            self.assertEqual(result['metrics']['moving']['frames'],1)

    def test_incomplete_actor_policy_rejected(self):
        with self.assertRaises(ValueError):
            matrix.route_quality({'playback':{'playback_complete':True,'errors':[],
                'finish':{'policy':{'qualified':True,'combat_during_recording':1}}}})

    def test_bounded_exterior_crossing_and_wrong_itinerary(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)
            rows=[{'event':'check','seq':1,'t_wall':1.,'cell_matches':True,
                   'actual_cell':'Esm3ExteriorCell:-3:-3','expected_cell':'Esm3ExteriorCell:-3:-3',
                   'actual_position':{'x':-20030,'y':-16410,'z':289},
                   'expected_position':{'x':-20030,'y':-16410,'z':289}},
                  {'event':'check','seq':2,'t_wall':1.2,'cell_matches':False,
                   'actual_cell':'Esm3ExteriorCell:-3:-2','expected_cell':'Esm3ExteriorCell:-3:-3',
                   'actual_position':{'x':-20032,'y':-16375,'z':289},
                   'expected_position':{'x':-20005,'y':-16389,'z':289}},
                  {'event':'check','seq':3,'t_wall':1.4,'cell_matches':True,
                   'actual_cell':'Esm3ExteriorCell:-3:-2','expected_cell':'Esm3ExteriorCell:-3:-2',
                   'actual_position':{'x':-20110,'y':-16280,'z':289},
                   'expected_position':{'x':-20110,'y':-16280,'z':289}}]
            def save():
                (path/'playback.jsonl').write_text('\n'.join(map(json.dumps,rows)))
            save()
            result=matrix.cell_crossings(path,1)
            self.assertEqual(result['qualification'],'BOUNDED_EXTERIOR_BOUNDARY_SKEW')
            self.assertEqual(result['mismatch_checks'],1)
            # The actual 0.2-second acquisition cadence and an ordinary frame
            # overshoot can put two MATCHING endpoints more than half a second
            # apart. Preserve that uncertainty without calling it a sustained
            # mismatch or permitting an undersampled crossing.
            rows[-1]['t_wall']=1.535; save()
            result=matrix.cell_crossings(path,1)
            window=result['windows'][0]
            self.assertAlmostEqual(window['observed_mismatch_seconds'],0)
            self.assertAlmostEqual(window['surrounding_sample_seconds'],.535)
            self.assertAlmostEqual(window['after_sample_gap_seconds'],.335)
            rows[-1]['t_wall']=1.4; save()
            # Position interpolates across a boundary before the reference's
            # earlier sampled cell label changes. It must remain explicitly
            # reported and confined to the recorded sample's travel distance.
            rows[1]['sample_interval']=.05
            rows[1]['guidance_command']={'speed':300}
            rows[1]['expected_position']['y']=-16383; save()
            self.assertEqual(matrix.cell_crossings(path,1)['reference_interpolation_skew_checks'],1)
            rows[1]['expected_position']['y']=-16350; save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)
            rows[1]['expected_position']['y']=-16389
            rows[-1]['t_wall']=2.; save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)
            rows[-1]['t_wall']=1.4
            rows[1]['actual_position']['x']-=100; save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)
            rows[1]['actual_position']['x']+=100
            rows[1]['actual_cell']='Esm3ExteriorCell:-4:-2';save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)

    def test_observed_sustained_cell_mismatch_still_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)
            rows=[]
            for seq,when,mismatch in ((1,1.,False),(2,1.2,True),
                    (3,1.4,True),(4,1.6,True),(5,1.8,True),(6,2.,False)):
                actual='Esm3ExteriorCell:-3:-3' if seq==1 else 'Esm3ExteriorCell:-3:-2'
                expected='Esm3ExteriorCell:-3:-2' if seq==6 else 'Esm3ExteriorCell:-3:-3'
                rows.append({'event':'check','seq':seq,'t_wall':when,'cell_matches':not mismatch,
                    'actual_cell':actual,'expected_cell':expected,
                    'actual_position':{'x':-20030,'y':-16410 if seq==1 else -16375,'z':289},
                    'expected_position':{'x':-20005,'y':-16389 if seq!=6 else -16280,'z':289}})
            (path/'playback.jsonl').write_text('\n'.join(map(json.dumps,rows)))
            with self.assertRaisesRegex(ValueError,'mismatch span'):
                matrix.cell_crossings(path,4)

    def test_cache_engagement_does_not_prove_visible_lod(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)
            (path/'openmw.log').write_text('active_consumed_draws=0 cache_fast_hits=5\n')
            result=matrix.engagement(path,{'lod cache hit admission':'true'},1)
            self.assertTrue(result['positive_work']['lod_cache_hit'])
            self.assertFalse(result['all_requested_positive'])
            (path/'openmw.log').write_text('active_consumed_draws=10 cache_fast_hits=5\n')
            self.assertTrue(matrix.engagement(path,{'lod cache hit admission':'true'},1)['all_requested_positive'])
            (path/'openmw.log').write_text('OptimizedMW persistent opaque packets: submitted=1d169f ordinary_branches=1693\n')
            self.assertFalse(matrix.engagement(path,{'ordinary opaque packets':'true'})['all_requested_positive'])
            (path/'openmw.log').write_text('active_consumed_draws=0 primitive_clone_bytes_avoided=1024\n')
            self.assertFalse(matrix.engagement(path,{'lod retain source indices':'true'},1)['all_requested_positive'])
            (path/'openmw.log').write_text('active_consumed_draws=10 primitive_clone_bytes_avoided=1024\n')
            self.assertTrue(matrix.engagement(path,{'lod retain source indices':'true'},1)['all_requested_positive'])
            (path/'openmw.log').write_text('command_uploads_avoided=0\n')
            self.assertFalse(matrix.engagement(path,{'indirect command reuse':'true'})['all_requested_positive'])
            (path/'openmw.log').write_text('command_uploads_avoided=25\n')
            self.assertTrue(matrix.engagement(path,{'indirect command reuse':'true'})['all_requested_positive'])


    def test_atomic_receipt_survives_a_real_windows_reader_lock(self):
        if matrix.benchmark_desktop.os.name!='nt':
            self.skipTest('Windows delete-sharing behavior')
        import threading, time
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'receipt.json'
            matrix.benchmark_desktop.save(path,{'generation':1})
            locked=threading.Event()
            def reader():
                with path.open('rb') as stream:
                    self.assertEqual(json.load(stream)['generation'],1)
                    locked.set(); time.sleep(.15)
            thread=threading.Thread(target=reader)
            thread.start(); self.assertTrue(locked.wait(1))
            matrix.benchmark_desktop.save(path,{'generation':2})
            thread.join()
            self.assertEqual(matrix.read_json(path)['generation'],2)
            self.assertFalse(path.with_suffix('.json.writing').exists())

    def test_private_window_lifetime_witness_survives_hide_before_exit(self):
        if matrix.benchmark_desktop.os.name != 'nt':
            self.skipTest('Windows owned private desktop lifecycle')
        import sys
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'private-lifecycle'
            code = matrix.benchmark_desktop.launch(sys.executable,
                [str(ROOT / 'benchmark-window-probe.py')], ROOT, output, 15)
            self.assertEqual(code, 0)
            receipt = matrix.read_json(output / 'DESKTOP-RECEIPT.json')
            self.assertTrue(receipt['owned_render_window_observed'])
            self.assertFalse(receipt['latest_private_windows_visible'])
            self.assertFalse(receipt['input_desktop_owned_visible_windows'])
            self.assertTrue(receipt['input_desktop_unchanged'])
            self.assertFalse(receipt['desktop_switched'])
            probe = json.loads((output / 'stdout.log').read_text().strip())
            self.assertEqual(probe['console_window'], 0)
            self.assertTrue(probe['visible_then_hidden_before_exit'])


if __name__=='__main__':unittest.main()
