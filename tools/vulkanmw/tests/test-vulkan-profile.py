import base64
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1] / 'vulkan-profile.py'
spec = importlib.util.spec_from_file_location('isolated_vk_profile', TOOL)
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileTests(unittest.TestCase):
    def test_settings_case_empty_chain_no_interpolation(self):
        self.assertEqual(profile.settings('[Post Processing]\nchain=\n[Game]\nx=50%\n'),
                         {('Post Processing', 'chain'): '', ('Game', 'x'): '50%'})

    def test_duplicate_settings_fail(self):
        with self.assertRaises(ValueError):
            profile.settings('[V3]\nx=1\nx=2')

    def test_semicolon_is_not_engine_comment(self):
        self.assertIn(('Video', ';framerate limit'), profile.settings('[Video]\n;framerate limit=45'))

    def test_environment_presence_controls_removed_not_zeroed(self):
        original = {'PATH': 'test', 'SystemRoot': 'C:/Windows', 'OPENMW_V3_X': '1',
                    'OPENMW_V4_LEGACY_HOST_RETENTION_CONTROL': '0', 'OSG_THREADING': 'x',
                    'Vk_INSTANCE_LAYERS': 'bad', '__GL_FOO': '1', 'VSG_FILE_PATH': 'x'}
        child, removed = profile.environment(original, {'OPENMW_V4_STATIC_FRUSTUM': '1'})
        self.assertEqual(child, {'PATH': 'test', 'SystemRoot': 'C:/Windows', 'OPENMW_V4_STATIC_FRUSTUM': '1'})
        self.assertEqual(len(removed), 6)
        self.assertIn('OSG_THREADING', original)

    def test_selection_keeps_quality_semantics_not_presets(self):
        defaults = {('V3', 'v3.6 performance profile'): 'true', ('Game', 'weapon sheathing'): 'false',
                    ('Shadows', 'shadow map resolution'): '1024', ('Video', 'renderer backend'): 'auto'}
        normal = {('V3', 'v3.6 performance profile'): 'true', ('Game', 'weapon sheathing'): 'true',
                  ('Shadows', 'shadow map resolution'): '2048', ('Video', 'renderer backend'): 'opengl',
                  ('Cells', 'optimizedmw compile scheduler mode'): '2'}
        selected, unknown = profile.select_settings(defaults, normal, {('V3', 'v3.6 performance profile'): 'false'})
        self.assertEqual(selected[('Game', 'weapon sheathing')], 'true')
        self.assertEqual(selected[('Shadows', 'shadow map resolution')], '2048')
        self.assertNotIn(('Video', 'renderer backend'), selected)
        self.assertEqual(unknown, ['[Cells] optimizedmw compile scheduler mode'])

    def test_missing_override_key_fails(self):
        with self.assertRaises(ValueError):
            profile.select_settings({}, {}, {('Video', 'imaginary'): '1'})

    def test_content_order_paths_and_replace(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cfg = root / 'openmw.cfg'
            cfg.write_text('config=child\nreplace=config\nreplace=content\ndata="../a& b"\n'
                           'content=a.esm\ncontent=b.esp\nuser-data=normal\nscript-run=unsafe.txt\n')
            result, removed = profile.content_snapshot(cfg, root, root)
            self.assertNotIn('config=', result)
            self.assertNotIn('script-run', result)
            self.assertIn('replace=content\n', result)
            self.assertTrue(result.endswith('content=a.esm\ncontent=b.esp\n'))
            self.assertIn(profile.dc.quoted(root / '../a b'), result)  # ampersand escape matches engine
            self.assertEqual(len(removed), 3)

    def test_unknown_content_and_unsafe_save_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for text in ('load-savegame=\n', 'new-unknown-mod-option=true\n', 'data="?unknown?foo"\n'):
                cfg = root / 'openmw.cfg'
                cfg.write_text(text)
                with self.assertRaises(ValueError):
                    profile.content_snapshot(cfg, root, root)

    def test_settings_roundtrip(self):
        text = profile.HERE.joinpath('vulkan-clean-settings.cfg').read_text()
        self.assertEqual(profile.settings(text), profile.settings(profile.serialized(profile.settings(text))))

    def test_real_package_definitions_and_prepare_verify_without_launch(self):
        # The source template supplies known keys for synthetic unit tests; actual package
        # validation is also performed by prepare against its decoded defaults.bin.
        defaults = profile.settings((profile.ROOT / 'files/settings-default.cfg').read_text())
        overrides = profile.settings((profile.HERE / 'vulkan-clean-settings.cfg').read_text())
        self.assertFalse(set(overrides) - set(defaults))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package, normal, output = root / 'runtime', root / 'normal', root / 'clean'
            package.mkdir()
            normal.mkdir()
            (package / 'resources').mkdir()
            (package / 'openmw.exe').write_bytes(b'unit-test-not-executable')
            (package / 'openmw.cfg').write_text('config="?userconfig?"\ndata=resources/vfs-mw\n')
            (package / 'defaults.bin').write_bytes(base64.b64encode(profile.serialized(defaults).encode()))
            (normal / 'openmw.cfg').write_text('content=first.esm\ncontent=last.esp\n')
            original = '[V3]\nv3.6 performance profile=true\n[Game]\nweapon sheathing=true\n'
            (normal / 'settings.cfg').write_text(original)
            with patch.object(profile.dc, 'normal_default', return_value=normal):
                manifest = profile.prepare(package / 'openmw.exe', normal, output)
            verified, effective = profile.verify(output)
            self.assertEqual(verified['command'], manifest['command'])
            self.assertNotIn(str(normal), manifest['command'])
            self.assertFalse((output / 'content/0/settings.cfg').exists())
            self.assertFalse((output / 'user-data/saves').exists())
            self.assertEqual(effective[('V3', 'v3.6 performance profile')], 'false')
            self.assertEqual((normal / 'settings.cfg').read_text(), original)
            self.assertNotIn('OPENMW_VK_NATIVE_SKELETAL_OBJECTS', manifest['controls'])
            self.assertNotIn('OPENMW_V4_SUBMIT_BREAKDOWN', manifest['controls'])
            snapshot_settings = output / 'content/0/settings.cfg'
            snapshot_settings.write_text('[V3]\nv3.6 performance profile=true\n')
            with self.assertRaises(ValueError):
                profile.verify(output)
            snapshot_settings.unlink()
            profile.verify(output)
            # Adding package-level settings must never silently contaminate later runs.
            (package / 'settings.cfg').write_text('[Video]\nrender scale=.5\n')
            with self.assertRaises(ValueError):
                profile.verify(output)

    def test_existing_output_or_package_settings_never_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'openmw.exe').write_bytes(b'test')
            with self.assertRaises(ValueError):
                profile.prepare(root / 'openmw.exe', root, root)
            self.assertEqual((root / 'openmw.exe').read_bytes(), b'test')


if __name__ == '__main__':
    unittest.main()
