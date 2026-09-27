"""No game/profile side effects: explicit same-executable animation control."""
import ast
import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'v4/cp4/architecture-benchmark.py'
spec = importlib.util.spec_from_file_location('benchmark', SOURCE)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class AnimationControlTest(unittest.TestCase):
    def test_helpers_are_defined_before_cli_entrypoint(self):
        module = ast.parse(SOURCE.read_text(encoding='utf-8'))
        entrypoint = next(node.lineno for node in module.body
                          if isinstance(node, ast.If) and '__name__' in ast.unparse(node.test))
        for node in module.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self.assertLess(node.lineno, entrypoint, node.name)

    def test_resolution_probe_replaces_instead_of_duplicates(self):
        result = benchmark.diagnostic_half_resolution('[Video]\nresolution x = 1920\nresolution y = 1080\n')
        self.assertEqual(result.count('resolution x ='), 1)
        self.assertIn('resolution x = 960\n', result)
        self.assertIn('resolution y = 540\n', result)
        with self.assertRaises(ValueError):
            benchmark.diagnostic_half_resolution('[Video]\nresolution x = 960\n')

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

    def test_resource_experiments_default_off(self):
        self.assertEqual(benchmark.resource_controls('vulkan'), {})
        self.assertEqual(benchmark.resource_controls('opengl'), {})

    def test_pose_validation_requires_native_arm_before_profile_creation(self):
        from types import SimpleNamespace
        with self.assertRaisesRegex(ValueError, 'requires --skeletal-objects'):
            benchmark.run(SimpleNamespace(renderer='vulkan', legacy_animation_capture=False,
                population_deltas=False, actor_streams=False, land_depth_occluders=False,
                skeletal_objects=False, validate_skeletal_poses=True))

    def test_resource_experiments_independent(self):
        self.assertEqual(benchmark.resource_controls('vulkan', True, False),
                         {'OPENMW_VK_POPULATION_DELTAS': '1'})
        self.assertEqual(benchmark.resource_controls('vulkan', False, True),
                         {'OPENMW_VK_ACTOR_STREAMS': '1'})
        self.assertEqual(len(benchmark.resource_controls('vulkan', True, True)), 2)
        self.assertEqual(benchmark.resource_controls('vulkan', False, False, True),
                         {'OPENMW_VK_LAND_DEPTH_OCCLUDERS': '1'})
        self.assertEqual(benchmark.resource_controls('vulkan', cached_object_admission=True),
                         {'OPENMW_VK_CACHED_OBJECT_ADMISSION': '1'})

    def test_resource_experiments_reject_opengl(self):
        self.assertEqual(benchmark.resource_controls('vulkan', skeletal_objects=True),
                         {'OPENMW_VK_NATIVE_SKELETAL_OBJECTS': '1'})
        with self.assertRaises(ValueError):
            benchmark.resource_controls('opengl', skeletal_objects=True)
        with self.assertRaises(ValueError):
            benchmark.resource_controls('opengl', True, False)
        with self.assertRaises(ValueError):
            benchmark.resource_controls('opengl', False, True)
        with self.assertRaises(ValueError):
            benchmark.resource_controls('opengl', False, False, True)
        with self.assertRaises(ValueError):
            benchmark.resource_controls('opengl', cached_object_admission=True)


if __name__ == '__main__':
    unittest.main()
