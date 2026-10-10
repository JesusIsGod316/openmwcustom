"""Installed normal-settings launcher contracts; no game process is started."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('normal_vulkan_launcher', ROOT / 'tools/vulkanmw/vulkan-play.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class NormalBenchmarkTests(unittest.TestCase):
    def test_packaging_has_only_two_public_vulkan_entrypoints(self):
        cmake = (ROOT / 'apps/openmw/CMakeLists.txt').read_text()
        start = cmake.index('if(OPENMW_ENABLE_V4_VULKAN_RUNTIME)', cmake.index('# VulkanMW packaging contract'))
        public = cmake[start:cmake.index('# Preserve the source-relative tool layout', start)]
        self.assertEqual(public.count('.bat'), 2)
        self.assertIn('START-VulkanMW-Test.bat', public)
        self.assertIn('START-VulkanMW-Benchmark.bat', public)
        root_cmake = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('if(WIN32 AND NOT OPENMW_ENABLE_V4_VULKAN_RUNTIME)', root_cmake)
        workflow = (ROOT / '.github/workflows/vulkanmw-producer-queues.yml').read_text()
        required = workflow[workflow.index('foreach ($required in @('):]
        required = required[:required.index(')) {')]
        self.assertIn('START-VulkanMW-Benchmark.bat', required)
        self.assertNotIn('START-VulkanMW-P3B-Test.bat', required)
        self.assertIn('tools/vulkanmw/vulkan-user-profile.py', required)
        self.assertIn('Experimental package still references the normal writable profile', workflow)
        self.assertIn('isolate_vulkan_profile("${OpenMW_BINARY_DIR}/openmw.cfg.install")', root_cmake)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.package = Path(self.temporary.name) / "package with ' spaces"
        self.normal = Path(self.temporary.name) / 'normal'
        (self.package / 'resources').mkdir(parents=True)
        self.normal.mkdir()
        (self.package / 'openmw.exe').write_bytes(b'fixture only -- not executable')
        (self.package / 'openmw.cfg').write_text('# local config\n', encoding='utf-8')
        (self.normal / 'openmw.cfg').write_text('user-data=' + launcher.dc.quoted(self.normal) + '\n', encoding='utf-8')
        (self.normal / 'settings.cfg').write_text('[Shadows]\nenable shadows = true\n', encoding='utf-8')
        (self.normal / 'save.omwsave').write_bytes(b'normal save')
        (self.normal / 'saves').mkdir()
        (self.normal / 'saves/fixture.omwsave').write_bytes(b'normal save')
        (self.normal / 'player_storage.bin').write_bytes(b'normal storage fixture')
        identity = {'source_head': '3993b098613b131ba431d7b2f57320a2c8a2ce60',
                    'executable_sha256': hashlib.sha256((self.package / 'openmw.exe').read_bytes()).hexdigest(),
                    'changed_source_files': {}}
        (self.package / 'source-changes.json').write_text(json.dumps(identity), encoding='utf-8')

    def argv(self, *options):
        return ['vulkan-play.py', '--package', str(self.package), '--user-config', str(self.normal), *options]

    def test_stage1_and_control_only_differ_by_stage1_switch(self):
        candidate = launcher.controls_for(persistent_populations=True)
        control = launcher.controls_for(persistent_populations=True, p3b_control=True)
        self.assertEqual(candidate, control | {'OPENMW_VK_PERSISTENT_POPULATIONS': '1'})
        for name in ('OPENMW_VK_GPU_SCENE_TABLES', 'OPENMW_VK_GPU_CULL_INDIRECT', 'OPENMW_VK_GPU_CULL_COMPACT',
                     'OPENMW_V4_INCREMENTAL_POPULATIONS', 'OPENMW_V4_INCREMENTAL_INSTANCES'):
            self.assertEqual(candidate.get(name), '1')
        p1 = launcher.controls_for(persistent_populations=True, p1_only=True)
        self.assertNotIn('OPENMW_VK_GPU_SCENE_TABLES', p1)
        self.assertNotIn('OPENMW_VK_GPU_CULL_INDIRECT', p1)
        self.assertNotIn('OPENMW_VK_PERSISTENT_POPULATIONS', p1)

    def test_dry_run_does_not_launch_or_rewrite_normal_data(self):
        original = {str(path.relative_to(self.normal)): path.read_bytes() for path in self.normal.rglob('*') if path.is_file()}
        with patch.object(sys, 'argv', self.argv('--benchmark', '--persistent-populations', '--dry-run')), \
                patch.object(launcher.subprocess, 'run') as run:
            launcher.main()
            run.assert_not_called()
        self.assertEqual(original, {str(path.relative_to(self.normal)): path.read_bytes() for path in self.normal.rglob('*') if path.is_file()})
        manifest = json.loads(next((self.package / 'Benchmarks').glob('manual-*/manifest.json')).read_text())
        self.assertEqual(manifest['controls']['OPENMW_VK_PERSISTENT_POPULATIONS'], '1')
        self.assertNotIn('OPENMW_VK_FRAME_PROFILE', manifest['controls'])
        self.assertIn('normal settings', manifest['settings_mode'])

    def test_binding_control_keeps_population_and_other_prerequisites_identical(self):
        candidate = launcher.controls_for(object_binding_updates=True)
        control = launcher.controls_for(object_binding_updates=True, binding_control=True)
        self.assertEqual(candidate, control | {'OPENMW_VK_OBJECT_BINDING_UPDATES': '1'})
        self.assertEqual(control.get('OPENMW_VK_PERSISTENT_POPULATIONS'), '1')
        p3b = launcher.controls_for(object_binding_updates=True, p3b_control=True)
        self.assertNotIn('OPENMW_VK_OBJECT_BINDING_UPDATES', p3b)
        self.assertNotIn('OPENMW_VK_PERSISTENT_POPULATIONS', p3b)
        self.assertEqual(p3b.get('OPENMW_VK_GPU_CULL_COMPACT'), '1')

    def test_manual_session_keeps_normal_settings_and_records_identity(self):
        original = {str(path.relative_to(self.normal)): path.read_bytes() for path in self.normal.rglob('*') if path.is_file()}
        calls = []
        def fake_run(command, **kwargs):
            calls.append(command)
            configs = [Path(command[index + 1]) for index, value in enumerate(command) if value == '--config']
            self.assertEqual(configs, [self.package / 'UserProfile'])
            overlay = configs[0]
            settings = launcher.profile.settings((overlay / 'settings.cfg').read_text())
            self.assertEqual(settings[('Shadows', 'enable shadows')], 'true')
            self.assertEqual(settings[('Video', 'renderer backend')], 'vulkan')
            self.assertEqual(settings[('Video', 'renderer fallback')], 'false')
            self.assertEqual(Path(command[command.index('--user-data') + 1]), overlay / 'user-data')
            self.assertEqual(Path(command[command.index('--data-local') + 1]), overlay / 'local-data')
            self.assertEqual((overlay / 'user-data/saves/fixture.omwsave').read_bytes(), b'normal save')
            # Simulate actual engine writes; normal bytes must remain untouched.
            (overlay / 'player_storage.bin').write_bytes(b'changed experimental storage')
            (overlay / 'user-data/saves/fixture.omwsave').write_bytes(b'changed experimental save')
            self.assertNotIn('OPENMW_V4_COARSE_POPULATION_REBUILD_CONTROL', kwargs['env'])
            self.assertEqual(kwargs['env']['OPENMW_VK_PERSISTENT_POPULATIONS'], '1')
            self.assertEqual(kwargs['env']['OPENMW_VK_FRAME_PROFILE'], '1')
            return SimpleNamespace(returncode=0)
        with patch.object(sys, 'argv', self.argv('--benchmark', '--persistent-populations', '--frame-profile')), \
                patch.dict(launcher.os.environ, {'OPENMW_V4_COARSE_POPULATION_REBUILD_CONTROL': '1'}), \
                patch.object(launcher.subprocess, 'run', side_effect=fake_run), self.assertRaises(SystemExit) as code:
            launcher.main()
        self.assertEqual(code.exception.code, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(original, {str(path.relative_to(self.normal)): path.read_bytes() for path in self.normal.rglob('*') if path.is_file()})
        session = next((self.package / 'Benchmarks').glob('manual-*'))
        manifest = json.loads((session / 'manifest.json').read_text())
        self.assertEqual(manifest['state'], 'exited')
        self.assertTrue(all(manifest['original_chain_unchanged'].values()))
        self.assertEqual(manifest['source']['executable_sha256'], launcher.dc.digest(self.package / 'openmw.exe'))
        self.assertTrue((session / 'console.log').exists())

    def test_private_storage_and_saves_persist_without_reseed(self):
        chain, _ = launcher.dc.inspect_chain(self.package, self.normal, launcher.dc.normal_default())
        private = launcher.private_profile.prepare(self.package, self.normal, chain, launcher.dc, launcher.profile)
        (private / 'player_storage.bin').write_bytes(b'private state')
        (private / 'user-data/saves/fixture.omwsave').write_bytes(b'private save')
        launcher.private_profile.prepare(self.package, self.normal, chain, launcher.dc, launcher.profile)
        self.assertEqual((private / 'player_storage.bin').read_bytes(), b'private state')
        self.assertEqual((private / 'user-data/saves/fixture.omwsave').read_bytes(), b'private save')

    def test_modified_config_cannot_reintroduce_normal_writes(self):
        chain, _ = launcher.dc.inspect_chain(self.package, self.normal, launcher.dc.normal_default())
        private = launcher.private_profile.prepare(self.package, self.normal, chain, launcher.dc, launcher.profile)
        path = private / 'openmw.cfg'
        original = path.read_text()
        for addition in ('config=' + launcher.dc.quoted(self.normal),
                         'user-data=' + launcher.dc.quoted(self.normal),
                         'data-local=' + launcher.dc.quoted(self.normal)):
            path.write_text(original + addition + '\n')
            with self.assertRaises(ValueError):
                launcher.private_profile.validate(self.package, self.normal, private, launcher.dc)

    def test_future_windows_package_config_is_private(self):
        path = self.package / 'generated.cfg'
        path.write_text('data-local="?userdata?data"\nuser-data="?userdata?"\nconfig="?userconfig?"\n')
        script = self.package / 'configure.cmake'
        script.write_text('include("' + (ROOT / 'cmake/IsolateVulkanProfile.cmake').as_posix() + '")\n'
                          + 'isolate_vulkan_profile("' + path.as_posix() + '")\n')
        result = subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('?userconfig?', path.read_text())
        self.assertNotIn('?userdata?', path.read_text())
        self.assertIn('config="?local?UserProfile"', path.read_text())

    def test_replaced_binary_fails_before_launch(self):
        (self.package / 'openmw.exe').write_bytes(b'replaced')
        with patch.object(sys, 'argv', self.argv('--dry-run')), patch.object(launcher.subprocess, 'run') as run, \
                self.assertRaisesRegex(SystemExit, 'no longer matches'):
            launcher.main()
        run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
