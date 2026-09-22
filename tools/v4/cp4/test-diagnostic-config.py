"""Regression tests for the two observed launcher failures and save isolation.

These exercise preparation/argument generation, not a real OpenMW launch.
"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import hashlib
import diagnosticconfig as config

class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cp4 config ')
        # Windows TEMP may contain an 8.3 alias (RUNNER~1). The production
        # preflight resolves directory identities; expectations must compare the
        # same canonical paths rather than failing on two spellings of one dir.
        self.root = Path(self.tmp.name).resolve()
        self.package = self.root / 'package'
        self.normal = self.root / 'normal'
        self.default = self.root / 'default'
        for p in (self.package, self.normal, self.default): p.mkdir()
        (self.package / 'openmw.cfg').write_text('config="?userconfig?"\n')
        (self.normal / 'openmw.cfg').write_text('content=Morrowind.esm\n')
    def tearDown(self): self.tmp.cleanup()
    def inspect(self): return config.inspect_chain(self.package, self.normal, self.default)
    def test_auto_user_config_is_not_followed_a_second_time(self):
        (self.default / 'openmw.cfg').write_text('load-savegame=DO-NOT-OPEN.omwsave\n')
        active, hashes = self.inspect()
        self.assertEqual(active, [self.package, self.normal])
        self.assertNotIn(str(self.default / 'openmw.cfg'), hashes)
        cmd = config.build_command(self.package / 'openmw.exe', self.normal, self.root / 'capture')
        self.assertIn('--replace=config', cmd)
        self.assertEqual(cmd.count(str(self.normal)), 1)
    def test_explicit_directory_alias_is_canonicalized(self):
        active, hashes = config.inspect_chain(self.package, self.normal / '..' / 'normal', self.default)
        self.assertEqual(active, [self.package, self.normal])
        self.assertIn(str(self.normal / 'openmw.cfg'), hashes)
    def test_empty_save_path_is_never_emitted(self):
        cmd = config.build_command(self.package / 'openmw.exe', self.normal, self.root / 'capture')
        self.assertNotIn('--load-savegame', cmd)
        self.assertEqual(cmd[cmd.index('--user-data') + 1], str(self.root / 'capture/user-data'))
    def test_empty_or_nonempty_inherited_autoload_refuses_preparation(self):
        for text in ('', '""', 'test.omwsave'):
            (self.normal / 'openmw.cfg').write_text('load-savegame=' + text + '\n')
            with self.assertRaisesRegex(ValueError, 'load-savegame'): self.inspect()
    def test_package_autoload_is_also_rejected(self):
        (self.package / 'openmw.cfg').write_text('load-savegame=\n')
        with self.assertRaises(ValueError): self.inspect()
    def test_nested_autoload_is_rejected(self):
        nested = self.normal / 'nested'; nested.mkdir()
        (self.normal / 'openmw.cfg').write_text('config=nested\n')
        (nested / 'openmw.cfg').write_text('load-savegame=normal.omwsave\n')
        with self.assertRaises(ValueError): self.inspect()
    def test_alias_cycle_is_rejected(self):
        (self.normal / 'openmw.cfg').write_text('config="../normal"\n')
        with self.assertRaisesRegex(ValueError, 'Repeated'): self.inspect()
    def test_unknown_token_is_rejected(self):
        (self.normal / 'openmw.cfg').write_text('config="?unknown?/child"\n')
        with self.assertRaisesRegex(ValueError, 'token'): self.inspect()
    def test_missing_config_directory_is_rejected(self):
        (self.normal / 'openmw.cfg').write_text('config=missing\n')
        with self.assertRaisesRegex(ValueError, 'missing'): self.inspect()
    def test_active_child_order_and_hashes_are_preserved(self):
        for name in ('first', 'second'):
            p = self.normal / name; p.mkdir(); (p / 'openmw.cfg').write_text('content=' + name + '.esp\n')
        (self.normal / 'openmw.cfg').write_text('config=first\nconfig=second\n')
        active, hashes = self.inspect()
        self.assertEqual(active, [self.package, self.normal, self.normal/'first', self.normal/'second'])
        self.assertEqual(len(hashes), 4)
    def test_openmw_ampersand_escaping_is_preserved(self):
        self.assertEqual(config.decode_config_path('"a&&b&\"c"'), 'a&b"c')
        self.assertEqual(config.decode_config_path('C:\\Directory With Spaces'), 'C:\\Directory With Spaces')
        for bad in ('', '""', '"unclosed', '"closed" trailing'):
            with self.assertRaises(ValueError): config.decode_config_path(bad)
    def test_current_package_identity_replaces_old_hardcoded_hash(self):
        exe = self.package / 'openmw.exe'; exe.write_bytes(b'new executable')
        commit = 'a' * 40
        (self.package / 'CP3E-TEST-IDENTITY.txt').write_text('commit=' + commit + '\n')
        (self.package / 'CP3E-PACKAGE-SHA256.txt').write_text(config.digest(exe) + '  openmw.exe\n')
        self.assertEqual(config.package_identity(exe, 'unrecorded'), commit)
        with self.assertRaises(ValueError): config.package_identity(exe, 'b' * 40)
        exe.write_bytes(b'other executable')
        with self.assertRaisesRegex(ValueError, 'hash'): config.package_identity(exe, commit)
    def test_missing_or_duplicate_executable_hash_is_rejected(self):
        exe = self.package / 'openmw.exe'; exe.write_bytes(b'exe')
        p = self.package / 'CP3E-PACKAGE-SHA256.txt'
        for text in ('', (config.digest(exe) + '  openmw.exe\n') * 2):
            p.write_text(text)
            with self.assertRaises(ValueError): config.package_identity(exe, 'unrecorded')
    def test_prepare_only_never_launches_and_preserves_normal_configuration(self):
        exe = self.package / 'openmw.exe'; exe.write_bytes(b'fixture')
        shaders = self.package / 'resources/shaders'; shaders.mkdir(parents=True)
        (shaders / 'test.vert').write_text('void main() {}')
        (shaders / 'shader-package.json').write_text(json.dumps({'schema':1,
            'overlay_sha256':'6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255',
            'files':{'test.vert':config.digest(shaders/'test.vert')}}))
        settings = self.normal/'settings.cfg'; settings.write_text('[V3]\nv3.6 performance profile = true\n')
        (self.normal/'input_v3.xml').write_text('input')
        saves = self.normal/'saves'; saves.mkdir(); (saves/'never-read.omwsave').write_bytes(b'save')
        original = settings.read_bytes()
        spec = importlib.util.spec_from_file_location('gameplay', Path(__file__).with_name('gameplay-diagnostics.py'))
        helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
        args = SimpleNamespace(executable=str(exe), user_config=str(self.normal), evidence_root=str(self.root/'captures'),
            diagnostics='standard', renderer='vulkan', dll_directory=[], osg_library_path=None,
            source_head='unrecorded', source_diff_sha256='unrecorded', prepare_only=True)
        with patch.object(helper, 'source_checkout', return_value=None), \
             patch.object(helper.subprocess, 'Popen') as launch:
            helper.launch(args)
            launch.assert_not_called()
        capture, = (self.root/'captures').iterdir()
        manifest = json.loads((capture/'manifest.json').read_text())
        self.assertEqual(manifest['state'], 'prepared_only')
        self.assertEqual(settings.read_bytes(), original)
        self.assertEqual((capture/'input_v3.xml').read_text(), 'input')
        self.assertFalse((capture/'user-data/saves').exists())
        self.assertNotIn('[V3]', (capture/'settings.cfg').read_text())

if __name__ == '__main__': unittest.main()
