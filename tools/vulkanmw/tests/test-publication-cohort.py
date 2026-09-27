import importlib.util
from pathlib import Path
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'run-publication-cohort.py'
spec = importlib.util.spec_from_file_location('publication_cohort', TOOL)
cohort = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cohort)


class CohortTests(unittest.TestCase):
    def test_control_has_no_presence_flags(self):
        self.assertEqual(cohort.selected('control'), {})

    def test_combined_contains_only_requested_repairs(self):
        self.assertEqual(cohort.selected('combined'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_CHUNK_TRANSACTIONS': '1',
            'OPENMW_VK_RESOURCE_INVENTORIES': '1'})

    def test_individual_and_pair_arms(self):
        self.assertEqual(cohort.selected('groups+inventories'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_RESOURCE_INVENTORIES': '1'})
        self.assertEqual(cohort.selected('transactions'), {'OPENMW_VK_CHUNK_TRANSACTIONS': '1'})
        self.assertEqual(cohort.selected('groups+transactions+inventories+tiles'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_CHUNK_TRANSACTIONS': '1',
            'OPENMW_VK_RESOURCE_INVENTORIES': '1', 'OPENMW_VK_TILED_LIGHTS': '1'})
        self.assertEqual(cohort.selected('groups+transactions+inventories+lod'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_CHUNK_TRANSACTIONS': '1',
            'OPENMW_VK_RESOURCE_INVENTORIES': '1', 'OPENMW_VK_PERSISTENT_EVALUATED_LOD': '1'})
        self.assertEqual(cohort.selected('groups+transactions+inventories+lod+particles'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_CHUNK_TRANSACTIONS': '1',
            'OPENMW_VK_RESOURCE_INVENTORIES': '1', 'OPENMW_VK_PERSISTENT_EVALUATED_LOD': '1',
            'OPENMW_VK_SPLIT_PARTICLE_CAPTURE': '1'})
        self.assertEqual(cohort.selected('groups+transactions+inventories+fastlights'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_CHUNK_TRANSACTIONS': '1',
            'OPENMW_VK_RESOURCE_INVENTORIES': '1', 'OPENMW_VK_TILED_LIGHTS': '1'})
        self.assertEqual(cohort.selected('groups+transactions+inventories+clustered'), {
            'OPENMW_VK_GROUP_PUBLICATION': '1', 'OPENMW_VK_CHUNK_TRANSACTIONS': '1',
            'OPENMW_VK_RESOURCE_INVENTORIES': '1'})

    def test_unknown_and_opengl_rejected(self):
        for value in ('opengl', '', 'groups+invalid', 'control+groups'):
            with self.assertRaises(ValueError):
                cohort.selected(value)

    def test_persistent_actors_preserve_cumulative_repairs(self):
        expected = cohort.selected('combined') | {'OPENMW_VK_PERSISTENT_ACTORS': '1'}
        self.assertEqual(cohort.selected('groups+transactions+inventories+actors'), expected)
        self.assertNotIn('OPENMW_VK_PERSISTENT_ACTORS', cohort.selected('combined'))
        self.assertEqual(cohort.selected('actors+placement-frustum'), {
            'OPENMW_VK_PERSISTENT_ACTORS': '1', 'OPENMW_VK_PLACEMENT_FRUSTUM': '1'})
        self.assertEqual(cohort.selected('particle-slots'), {'OPENMW_VK_RETAIN_PARTICLE_SLOTS': '1'})

    def test_inherited_experiments_do_not_contaminate_control(self):
        child, _ = cohort.profile.environment(
            {'PATH': 'unchanged', 'OPENMW_VK_GROUP_PUBLICATION': '0', 'Vk_INSTANCE_LAYERS': 'external'},
            cohort.selected('control'))
        self.assertEqual(child, {'PATH': 'unchanged'})

    def test_producers_and_shadow_recording_are_independent(self):
        self.assertEqual(cohort.selected('change-driven'), {'OPENMW_VK_CHANGE_DRIVEN_OBJECTS': '1'})
        self.assertEqual(cohort.selected('shadow-record'), {'OPENMW_VK_PARALLEL_SHADOW_RECORD': '1'})
        self.assertEqual(cohort.selected('change-driven+shadow-record'), {
            'OPENMW_VK_CHANGE_DRIVEN_OBJECTS': '1', 'OPENMW_VK_PARALLEL_SHADOW_RECORD': '1'})

    def test_producer_queue_arm_is_independent(self):
        base = 'groups+transactions+inventories+actors+admission+change-driven'
        control = cohort.selected(base)
        candidate = cohort.selected(base + '+queues')
        self.assertEqual(candidate, control | {'OPENMW_VK_PRODUCER_DIRTY_QUEUES': '1'})
        self.assertNotIn('OPENMW_VK_PRODUCER_DIRTY_QUEUES', control)
        self.assertNotIn('OPENMW_VK_PARALLEL_SHADOW_RECORD', candidate)
        self.assertNotIn('OPENMW_VK_TILED_LIGHTS', candidate)

    def test_overlay_diagnostic_is_explicit_and_process_local(self):
        parent = {'PATH': 'unchanged', 'DISABLE_RTSS_LAYER': '1'}
        child, removed = cohort.profile.environment(parent, cohort.selected('actors'))
        self.assertNotIn('DISABLE_RTSS_LAYER', child)
        self.assertIn('DISABLE_RTSS_LAYER', removed)
        child, _ = cohort.profile.environment(parent, cohort.selected('actors+no-overlay'))
        self.assertEqual(child['DISABLE_RTSS_LAYER'], '1')
        self.assertEqual(child['OPENMW_VK_PERSISTENT_ACTORS'], '1')
        self.assertEqual(parent, {'PATH': 'unchanged', 'DISABLE_RTSS_LAYER': '1'})


if __name__ == '__main__':
    unittest.main()
