import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('archive_source', Path(__file__).with_name('archive_source.py'))
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)

class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'repo'
        self.root.mkdir()
        archive.git(self.root, 'init', '--quiet')
        archive.git(self.root, 'config', 'user.name', 'Fixture')
        archive.git(self.root, 'config', 'user.email', 'fixture@example.invalid')
        verifier = self.root / 'tools/v4/V4-CP0A-Verify-Materialized-Generated-Outputs.py'
        verifier.parent.mkdir(parents=True)
        verifier.write_text('print("fixture materialization passes")\n')
        archive.git(self.root, 'add', '.')
        archive.git(self.root, 'commit', '--quiet', '-m', 'fixture')
        self.sha = archive.git(self.root, 'rev-parse', 'HEAD')
        self.out = Path(self.temp.name) / 'archive'
        self.tags = {'test-precleanup': self.sha, 'test-parked': self.sha}
        self.addCleanup(patch.stopall)
        patch.object(archive, 'BASE', self.sha).start()
        patch.object(archive, 'TAGS', self.tags).start()

    def test_complete_restore(self):
        result = archive.create_archive(self.root, self.out)
        self.assertEqual(result['empty_repository_restore'], 'PASS')
        self.assertEqual(len(result['references']), 2)
        self.assertEqual(result['bundle_sha256'], archive.sha256(self.out / result['bundle']))
        self.assertTrue((self.out / 'SHA256SUMS').is_file())
        self.assertEqual(archive.git(self.root, 'status', '--porcelain'), '')

    def test_dirty_tracked(self):
        (self.root / 'tools/v4/V4-CP0A-Verify-Materialized-Generated-Outputs.py').write_text('changed')
        with self.assertRaisesRegex(RuntimeError, 'dirty'):
            archive.create_archive(self.root, self.out)
        self.assertFalse(self.out.exists())

    def test_untracked(self):
        (self.root / 'private-wip.txt').write_text('not archived')
        with self.assertRaisesRegex(RuntimeError, 'dirty'):
            archive.create_archive(self.root, self.out)

    def test_output_inside_checkout(self):
        with self.assertRaisesRegex(RuntimeError, 'outside'):
            archive.create_archive(self.root, self.root / 'backup')

    def test_never_overwrite_backup(self):
        self.out.mkdir()
        (self.out / 'valuable').write_text('preserve')
        with self.assertRaises(FileExistsError):
            archive.create_archive(self.root, self.out)
        self.assertEqual((self.out / 'valuable').read_text(), 'preserve')

    def test_reject_lightweight_tag(self):
        archive.git(self.root, 'tag', 'test-precleanup', self.sha)
        with self.assertRaisesRegex(RuntimeError, 'does not match'):
            archive.create_archive(self.root, self.out)

    def test_corrupt_bundle_fails_empty_restore(self):
        archive.create_archive(self.root, self.out)
        bundle = self.out / 'OpimizedMW-GL-P0-precleanup.bundle'
        bundle.write_bytes(b'not a bundle')
        empty = Path(self.temp.name) / 'empty'
        empty.mkdir()
        archive.git(empty, 'init', '--quiet')
        with self.assertRaises(subprocess.CalledProcessError):
            archive.git(empty, 'bundle', 'verify', str(bundle))

if __name__ == '__main__':
    unittest.main()
