"""Ordering guards complement (never replace) the real VSG/GPU regression."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]

class P3GraphBoundary(unittest.TestCase):
    def test_finalizer_precedes_inventory_and_follows_conformance(self):
        source = (ROOT / 'components/render/backend/vsg/staticassetconformance.cpp').read_text()
        start = source.index('StaticRealizationResult realizeStaticAssetConformant(')
        source = source[start:]
        self.assertLess(source.index('result.root = routedRoot'), source.index('beforeSeal(*result.root)'))
        self.assertLess(source.index('beforeSeal(*result.root)'), source.index('sealPipelineInventory(result.root)'))
        self.assertIn('result.stats.runtimeContextEffects == 0', source)
        self.assertIn('result.stats.unsupportedTextureBindings == 0', source)

    def test_inventory_accessor_has_explicit_return_type(self):
        source = (ROOT / 'components/render/backend/vsg/pipelineinventory.hpp').read_text()
        self.assertIn('const std::vector<vsg::ref_ptr<vsg::GraphicsPipeline>>& pipelines() const', source)
        self.assertNotIn('const auto& pipelines()', source)
        self.assertIn('inventory->pipelines()', source)

    def test_host_rewrites_only_in_finalizer(self):
        source = (ROOT / 'components/render/backend/vsg/vsgruntimehost.cpp').read_text()
        self.assertEqual(source.count('enableGpuPopulationCull(world, plan'), 1)
        start = source.index('const StaticGraphFinalizer finalizePopulation')
        end = source.index('if (reuseAsset) realized.root', start)
        self.assertIn('enableGpuPopulationCull(world, plan, graphicsRoot', source[start:end])
        self.assertIn('plan.coordinateOrigin, 1.0f, false, finalizePopulation)', source[end:])
        self.assertIn('sealPipelineInventory(realized.root)', source[end:])
        self.assertNotIn('enableGpuPopulationCull(world, plan, *realized.root', source)

    def test_no_silent_empty_or_partial_draw_graph(self):
        source = (ROOT / 'components/render/backend/vsg/gpupopulationcull.cpp').read_text()
        self.assertIn('found no rewritable draws; finalize before inventory sealing', source)
        self.assertIn('draws.size() != plan.asset.draws.size()', source)
        self.assertIn('draw->instanceCount != placementCount || draw->firstInstance != 0', source)
        self.assertIn('limits.maxDrawIndirectCount', source)

    def test_capabilities_requested_before_host_creates_device(self):
        source = (ROOT / 'components/render/backend/vsg/vsgruntimebootstrap.cpp').read_text()
        for feature in ('multiDrawIndirect', 'drawIndirectFirstInstance'):
            self.assertIn('supported.' + feature, source)
            position = source.index('traits->deviceFeatures->get().' + feature + ' = VK_TRUE')
            self.assertLess(position, source.index('std::make_unique<VsgRuntimeHost>'))

    def test_real_vsg_and_gpu_fixtures_are_mandatory(self):
        windows = (ROOT / 'tools/v4/cp3b3/CMakeLists.txt').read_text()
        self.assertIn('openmw-vulkan-gpu-population-routing-smoke', windows)
        gpu = (ROOT / 'tools/vulkanmw/tests/p3-gpu/CMakeLists.txt').read_text()
        self.assertIn('gpupopulationcull.cpp', gpu)
        self.assertIn('P3_GPU_PIXEL_TESTS=1', gpu)
        self.assertIn('p3-real-gpu ${inventory} --gpu', gpu)

if __name__ == '__main__':
    unittest.main()
