"""Clock, context and pacing checks independent of game timing noise."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).parent
spec=importlib.util.spec_from_file_location('matrix',ROOT/'benchmark_matrix.py')
matrix=importlib.util.module_from_spec(spec);spec.loader.exec_module(matrix)


class Benchmarks(unittest.TestCase):
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
            rows[-1]['t_wall']=2.; save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)
            rows[-1]['t_wall']=1.4
            rows[1]['actual_position']['x']-=100; save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)
            rows[1]['actual_position']['x']+=100
            rows[1]['actual_cell']='Esm3ExteriorCell:-4:-2';save()
            with self.assertRaises(ValueError):matrix.cell_crossings(path,1)

    def test_cache_engagement_does_not_prove_visible_lod(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)
            (path/'openmw.log').write_text('active_consumed_draws=0 cache_fast_hits=5\n')
            result=matrix.engagement(path,{'lod cache hit admission':'true'},1)
            self.assertTrue(result['positive_work']['lod_cache_hit'])
            self.assertFalse(result['all_requested_positive'])
            (path/'openmw.log').write_text('active_consumed_draws=10 cache_fast_hits=5\n')
            self.assertTrue(matrix.engagement(path,{'lod cache hit admission':'true'},1)['all_requested_positive'])


if __name__=='__main__':unittest.main()
