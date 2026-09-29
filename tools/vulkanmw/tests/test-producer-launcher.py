"""Exercise the actual installed launcher layout without launching the game."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[3]
HEAD = '348bfe6b087b7c8b83738e12a11a7e10f9e592ef'


class ProducerLauncherTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.package = Path(self.tmp.name) / "package with ' spaces"
        for sub in ('tools/vulkanmw', 'tools/v4/cp4'):
            (self.package / sub).mkdir(parents=True)
        for name in ('run-producer-queues.py', 'run-publication-cohort.py', 'vulkan-profile.py',
                     'vulkan-clean-settings.cfg', 'frame-profile-report.py'):
            shutil.copy2(ROOT / 'tools/vulkanmw' / name, self.package / 'tools/vulkanmw' / name)
        for name in ('diagnosticconfig.py', 'gameplay-diagnostics.py', 'architecture-benchmark.lua'):
            shutil.copy2(ROOT / 'tools/v4/cp4' / name, self.package / 'tools/v4/cp4' / name)
        self.script = self.package / 'tools/vulkanmw/run-producer-queues.py'
        spec = importlib.util.spec_from_file_location('installed_producer_launcher', self.script)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        (self.package / 'openmw.exe').write_bytes(b'not executable -- no game is launched by fixture')
        (self.package / 'VULKANMW-PRODUCER-TEST.txt').write_text('source_head=' + HEAD)
        self.normal = Path(self.tmp.name) / 'normal'
        self.normal.mkdir()
        (self.normal / 'settings.cfg').write_text('normal settings must remain unchanged')
        (self.normal / 'save.omwsave').write_bytes(b'normal save must remain unchanged')

    def test_installed_layout_and_default_package(self):
        # The imported launcher canonicalizes __file__; TEMP may use an 8.3
        # spelling on Windows. Verify directory identity, not path spelling.
        self.assertTrue(self.module.HERE.parents[1].samefile(self.package))
        result = subprocess.run([sys.executable, str(self.script), '--help'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--single', result.stdout)
        self.assertIn('--supported-continuous', result.stdout)
        self.assertIn('--gpu-scene-tables', result.stdout)
        self.assertIn('--gpu-cull-indirect', result.stdout)
        self.assertEqual(list(self.package.glob('source-changes.json')), [])

    def test_identity_is_from_manifest_and_rechecks_executable(self):
        source = self.module.source_identity(self.package)
        self.assertEqual(source['source_head'], HEAD)
        self.assertEqual(self.module.source_identity(self.package), source)
        (self.package / 'openmw.exe').write_bytes(b'replaced exe')
        with self.assertRaisesRegex(ValueError, 'no longer matches'):
            self.module.source_identity(self.package)

    def test_missing_or_invalid_source_identity_fails_closed(self):
        (self.package / 'VULKANMW-PRODUCER-TEST.txt').unlink()
        with self.assertRaisesRegex(ValueError, 'lacks source provenance'):
            self.module.source_identity(self.package)
        (self.package / 'VULKANMW-PRODUCER-TEST.txt').write_text('source_head=latest')
        with self.assertRaises(ValueError):
            self.module.source_identity(self.package)

    def run_sequence(self, single=None, failing=False, supported_continuous=False,
                     gpu_scene_tables=False, gpu_cull_indirect=False):
        before = {p.name: p.read_bytes() for p in self.normal.iterdir()}
        calls = []
        def fake_run(package, user_config, source, name, arm):
            self.assertTrue(user_config.samefile(self.normal))
            self.assertEqual(source['source_head'], HEAD)
            calls.append(arm)
            target = package / 'Benchmarks' / name
            target.mkdir(parents=True)
            (target / 'manifest.json').write_text(json.dumps({'arm': arm}))
            if failing:
                raise RuntimeError('synthetic failed launch; keep evidence')
        argv = [str(self.script), '--package', str(self.package), '--user-config', str(self.normal)]
        if single:
            argv += ['--single', single]
        if supported_continuous:
            argv += ['--supported-continuous']
        if gpu_scene_tables:
            argv += ['--gpu-scene-tables']
        if gpu_cull_indirect:
            argv += ['--gpu-cull-indirect']
        with patch.object(sys, 'argv', argv), patch.object(self.module.cohort, 'run', side_effect=fake_run):
            if failing:
                with self.assertRaisesRegex(RuntimeError, 'synthetic'):
                    self.module.main()
            else:
                self.module.main()
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.normal.iterdir()})
        archives = list((self.package / 'Benchmarks').glob('*.zip'))
        self.assertEqual(len(archives), 1)
        with zipfile.ZipFile(archives[0]) as archive:
            self.assertEqual(len(archive.namelist()), len(calls))
            for name in archive.namelist():
                self.assertFalse(name.startswith(('/', '../')))
        return calls

    def test_abba_only_changes_queue_control(self):
        calls = self.run_sequence()
        self.assertEqual(calls, [self.module.BASE, self.module.CANDIDATE, self.module.CANDIDATE, self.module.BASE])
        baseline = self.module.cohort.selected(calls[0])
        candidate = self.module.cohort.selected(calls[1])
        self.assertEqual(candidate, baseline | {'OPENMW_VK_PRODUCER_DIRTY_QUEUES': '1'})

    def test_supported_continuous_abba_changes_only_supported_control(self):
        calls = self.run_sequence(supported_continuous=True)
        self.assertEqual(calls, [self.module.SUPPORTED_BASE, self.module.SUPPORTED_CANDIDATE,
                                 self.module.SUPPORTED_CANDIDATE, self.module.SUPPORTED_BASE])
        baseline = self.module.cohort.selected(calls[0])
        candidate = self.module.cohort.selected(calls[1])
        self.assertEqual(candidate, baseline | {'OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS': '1'})
        self.assertEqual(baseline.get('OPENMW_VK_PRODUCER_DIRTY_QUEUES'), '1')
        self.assertEqual(baseline.get('OPENMW_VK_SPLIT_PARTICLE_CAPTURE'), '1')

    def test_gpu_scene_table_abba_changes_only_p2_control(self):
        calls = self.run_sequence(gpu_scene_tables=True)
        self.assertEqual(calls, [self.module.GPU_TABLE_BASE, self.module.GPU_TABLE_CANDIDATE,
                                 self.module.GPU_TABLE_CANDIDATE, self.module.GPU_TABLE_BASE])
        baseline = self.module.cohort.selected(calls[0])
        candidate = self.module.cohort.selected(calls[1])
        self.assertEqual(candidate, baseline | {'OPENMW_VK_GPU_SCENE_TABLES': '1'})
        self.assertEqual(baseline.get('OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS'), '1')
        self.assertEqual(baseline.get('OPENMW_VK_PRODUCER_DIRTY_QUEUES'), '1')

    def test_gpu_cull_indirect_abba_changes_only_p3_control(self):
        calls = self.run_sequence(gpu_cull_indirect=True)
        self.assertEqual(calls, [self.module.GPU_CULL_BASE, self.module.GPU_CULL_CANDIDATE,
                                 self.module.GPU_CULL_CANDIDATE, self.module.GPU_CULL_BASE])
        baseline = self.module.cohort.selected(calls[0])
        candidate = self.module.cohort.selected(calls[1])
        self.assertEqual(candidate, baseline | {'OPENMW_VK_GPU_CULL_INDIRECT': '1'})
        self.assertEqual(baseline.get('OPENMW_VK_GPU_SCENE_TABLES'), '1')
        self.assertEqual(baseline.get('OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS'), '1')

    def test_failure_keeps_evidence_and_preserves_normal_data(self):
        self.assertEqual(len(self.run_sequence(failing=True)), 1)

    def test_single_smoke_is_explicit(self):
        self.assertEqual(self.run_sequence('candidate'), [self.module.CANDIDATE])


if __name__ == '__main__':
    unittest.main()
