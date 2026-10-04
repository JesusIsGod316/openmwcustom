#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("package_qualification", Path(__file__).with_name("qualify-asset-paging-package.py"))
Q = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(Q)


class PackageInputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="p9-package-qualification-")
        self.root = Path(self.temp.name)
        self.package = self.root / "assembled package"
        shaders = self.package / "resources" / "shaders"
        for name in ("compatibility/objects.vert", "compatibility/objects.frag", "lib/core/vertex.glsl"):
            file = shaders / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("ordinary fixture bytes " + name, encoding="utf-8")
        (self.package / "defaults.bin").write_bytes(b"fixture defaults")
        (self.package / "resources" / "version").write_text("OpenMW\n" + "a" * 40 + "\n")
        (self.package / "resources" / "nonshader-resource").write_bytes(b"retained resource")
        self.config, self.settings = self.root / "captured.cfg", self.root / "captured-settings.cfg"
        self.config.write_bytes(b"data=retained\n")
        self.settings.write_bytes(b"[Shadows]\nenable shadows = true\n")
        self.workspace = self.root / "private evidence"

    def tearDown(self):
        self.temp.cleanup()

    def prepare(self):
        return Q.prepare(self.package, self.config, self.settings, self.workspace)

    def test_exact_copy_and_read_only_verification(self):
        receipt = self.prepare()
        result = Q.verify(receipt)
        before = result.read_bytes(), result.stat().st_mtime_ns
        Q.verify(receipt, read_only=True)
        self.assertEqual(before, (result.read_bytes(), result.stat().st_mtime_ns))
        value = json.loads(receipt.read_text())
        self.assertEqual(value["status"], "PACKAGE_INPUTS_FROZEN_NOT_RENDER_QUALIFIED")
        self.assertFalse(value["synthetic_shader_certificate"])
        self.assertIn("nonshader-resource", value["resource_files"])

    def test_reused_workspace_keeps_all_evidence(self):
        receipt = self.prepare()
        before = receipt.read_bytes()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(before, receipt.read_bytes())

    def test_package_dependency_mutation_blocks(self):
        receipt = self.prepare()
        (self.package / "resources/shaders/lib/core/vertex.glsl").write_text("changed package")
        with self.assertRaises(ValueError):
            Q.verify(receipt)

    def test_nonshader_resource_mutation_blocks(self):
        receipt = self.prepare()
        (self.package / "resources/nonshader-resource").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            Q.verify(receipt)

    def test_capture_settings_mutation_blocks(self):
        receipt = self.prepare()
        self.settings.write_text("changed settings")
        with self.assertRaises(ValueError):
            Q.verify(receipt)

    def test_unrestored_private_shader_blocks(self):
        receipt = self.prepare()
        private = self.workspace / "shaders/lib/core/vertex.glsl"
        original = private.read_bytes()
        private.write_bytes(original + b"\nmutation")
        with self.assertRaises(ValueError):
            Q.verify(receipt)
        private.write_bytes(original)
        self.assertTrue(Q.verify(receipt).is_file())

    def test_missing_dependency_refuses_before_copy(self):
        (self.package / "resources/shaders/lib/core/vertex.glsl").unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.workspace.exists())

    def test_read_only_cannot_create_an_unchanged_receipt(self):
        receipt = self.prepare()
        with self.assertRaises(ValueError):
            Q.verify(receipt, read_only=True)
        self.assertFalse((self.workspace / "PACKAGE-QUALIFICATION-UNCHANGED.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
