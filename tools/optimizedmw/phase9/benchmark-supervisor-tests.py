import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import benchmark_supervisor as host


class SupervisorTests(unittest.TestCase):
    def run_fixture(self, matrix_state='completed', code=0, released=True, held=False, busy=0, memory_error=False):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); out=root/'Run'; checkpoint=root/'CP.json'; plan=root/'Plan.json'
            host.save(checkpoint, {'runtime_reservation':{'launch_policy':'Released' if released else 'Reserved'}})
            host.save(plan, {'game_directory':str(root/'Game'), 'cases':[]})
            if held:(root/'OVERNIGHT-HOLD.txt').write_text('human hold')
            class Kernel:
                def CloseHandle(self, unused):pass
            class Child:
                pid=1234; returncode=code
                def poll(self):return code
            def launch(*args, **kwargs):
                (out/'Matrix').mkdir()
                host.save(out/'Matrix/MATRIX.json',{'state':matrix_state,'error':'raw refusal' if code else ''})
                return Child()
            args=argparse.Namespace(output=str(out),plan=str(plan),checkpoint=str(checkpoint))
            sample={'qualified':True,'physical_available_bytes':8*1024**3,'commit_spare_bytes':8*1024**3}
            with patch.object(host,'mutex',return_value=(Kernel(),1)),patch.object(host,'occupied',return_value=busy),\
                 patch.object(host,'memory_floor',side_effect=RuntimeError('sample unavailable') if memory_error else None,return_value=sample),\
                 patch.object(host.subprocess,'Popen',side_effect=launch) as popen:
                if memory_error:
                    with self.assertRaisesRegex(RuntimeError,'sample unavailable'):host.work(args)
                    result=1
                else:result=host.work(args)
                return result,host.read(out/'SUPERVISOR.json'),host.read(checkpoint),popen.call_count

    def test_completed_requires_exact_matrix_terminal(self):
        code,state,cp,launches=self.run_fixture()
        self.assertEqual(code,0);self.assertEqual(state['state'],'completed');self.assertEqual(launches,1)
        self.assertFalse(state['performance_qualified']);self.assertIsNone(cp['active_controller_pid'])

    def test_zero_exit_without_completion_is_failure(self):
        code,state,cp,_=self.run_fixture(matrix_state='running')
        self.assertEqual(code,1);self.assertEqual(state['state'],'failed');self.assertIsNone(cp['active_job'])

    def test_failed_matrix_preserves_reason(self):
        code,state,cp,_=self.run_fixture(matrix_state='failed',code=1)
        self.assertEqual(code,1);self.assertEqual(state['reason'],'raw refusal');self.assertIsNone(cp['active_matrix'])

    def test_human_reservation_defers(self):
        code,state,_,launches=self.run_fixture(released=False)
        self.assertEqual(code,2);self.assertEqual(state['state'],'deferred');self.assertEqual(launches,0)

    def test_hold_defers(self):
        code,state,_,launches=self.run_fixture(held=True)
        self.assertEqual(code,2);self.assertEqual(state['state'],'deferred');self.assertEqual(launches,0)

    def test_occupied_reserved_slot_is_not_stopped(self):
        code,state,_,launches=self.run_fixture(busy=1,released=False)
        self.assertEqual(code,2);self.assertEqual(state['state'],'deferred');self.assertEqual(launches,0)

    def test_invalid_memory_cannot_launch(self):
        code,state,cp,launches=self.run_fixture(memory_error=True)
        self.assertEqual(state['state'],'failed');self.assertEqual(launches,0);self.assertIsNone(cp['active_job'])


if __name__=='__main__':unittest.main()
