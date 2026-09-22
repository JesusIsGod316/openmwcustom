import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location('shader_resources', Path(__file__).with_name('shader_resources.py'))
resources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resources)


class ShaderPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='shader package test ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        (self.source / 'compatibility').mkdir(parents=True)
        (self.source / 'compatibility/debug.vert').write_text('#include "lib/core/vertex.h.glsl"\n')
        self.base = self.root / 'files.txt'
        self.base.write_text('compatibility/debug.vert\n')
        self.overlay = self.root / 'overlay.zip'
        with zipfile.ZipFile(self.overlay, 'w') as archive:
            archive.writestr('lib/core/vertex.h.glsl', '@link "lib/core/vertex.glsl" if @condition\n#include "local.glsl"\n')
            archive.writestr('lib/core/vertex.glsl', 'void fixture() {}\n')
            archive.writestr('lib/core/local.glsl', '// fixture\n')
        checksum = resources.digest(self.overlay.read_bytes())
        self.pin = patch.object(resources, 'OVERLAY_SHA256', checksum)
        self.pin.start()
        self.addCleanup(self.pin.stop)
        self.destination = self.root / 'runtime'

    def stage(self):
        return resources.stage(self.source, self.base, self.overlay, self.destination)

    def test_complete_package_and_relative_includes(self):
        self.assertEqual(self.stage()['files'], 4)
        self.assertEqual(resources.verify(self.destination)['files'], 4)

    def test_missing_overlay_rejected_and_incremental_stage_repairs(self):
        self.stage()
        (self.destination / 'lib/core/vertex.h.glsl').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing shader resource.*vertex.h.glsl'):
            resources.verify(self.destination)
        self.assertEqual(self.stage()['files'], 4)

    def test_changed_shader_rejected(self):
        self.stage()
        (self.destination / 'compatibility/debug.vert').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'differs from staged package'):
            resources.verify(self.destination)

    def test_missing_conditional_link_rejected(self):
        self.stage()
        (self.destination / 'lib/core/vertex.glsl').unlink()
        with self.assertRaisesRegex(ValueError, 'missing @link'):
            resources.verify(self.destination)

    def test_missing_transitive_include_rejected(self):
        self.stage()
        (self.destination / 'lib/core/local.glsl').unlink()
        with self.assertRaisesRegex(ValueError, 'resolved lib/core/local.glsl'):
            resources.verify(self.destination)

    def test_cyclic_includes_rejected(self):
        self.stage()
        (self.destination / 'lib/core/local.glsl').write_text('#include "vertex.h.glsl"\n')
        with self.assertRaisesRegex(ValueError, 'Cyclic shader include'):
            resources.verify(self.destination)

    def test_bad_archive_rejected_before_writes(self):
        self.overlay.write_bytes(b'invalid archive')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.stage()
        self.assertFalse(self.destination.exists())

    def test_missing_manifest_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Missing shader package manifest'):
            resources.verify(self.destination)

    def test_path_escape_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsafe shader package path'):
            resources.safe_path(self.destination, '../outside')

    def test_overlay_wins_and_unrelated_files_preserved(self):
        (self.source / 'lib/core').mkdir(parents=True)
        (self.source / 'lib/core/local.glsl').write_text('base version')
        self.base.write_text(self.base.read_text() + 'lib/core/local.glsl\n')
        self.destination.mkdir()
        (self.destination / 'unrelated.txt').write_text('preserve')
        self.stage()
        self.assertEqual((self.destination / 'lib/core/local.glsl').read_text(), '// fixture\n')
        self.assertEqual((self.destination / 'unrelated.txt').read_text(), 'preserve')


if __name__ == '__main__':
    unittest.main()
