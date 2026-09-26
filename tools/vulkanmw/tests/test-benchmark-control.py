"""No game/profile side effects: explicit same-executable animation control."""
import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'v4/cp4/architecture-benchmark.py'
spec = importlib.util.spec_from_file_location('benchmark', SOURCE)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class AnimationControlTest(unittest.TestCase):
    def test_native_default(self):
        self.assertEqual(benchmark.animation_controls('vulkan', False), {})

    def test_explicit_legacy_arm(self):
        self.assertEqual(benchmark.animation_controls('vulkan', True),
                         {'OPENMW_V4_LEGACY_ANIMATION_CAPTURE_CONTROL': '1'})

    def test_opengl_unchanged(self):
        self.assertEqual(benchmark.animation_controls('opengl', False), {})

    def test_opengl_rejects_misleading_control(self):
        with self.assertRaises(ValueError):
            benchmark.animation_controls('opengl', True)

    def test_invalid_control_before_profile_creation(self):
        # Deliberately no other arguments: rejection must precede profile setup.
        from types import SimpleNamespace
        with self.assertRaisesRegex(ValueError, 'only meaningful for Vulkan'):
            benchmark.run(SimpleNamespace(renderer='opengl', legacy_animation_capture=True))


if __name__ == '__main__':
    unittest.main()
