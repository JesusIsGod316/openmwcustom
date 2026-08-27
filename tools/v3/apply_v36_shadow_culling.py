import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.6 shadow match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.6 shadow patched {rel} ({count} match(es))")


# Settings::cells() is declared in values.hpp, not in the category definition itself.
# The GPU-profiler layer adds the category include to shadow.cpp; complete the accessor include
# here before the shadow experiment consumes the V3.6 Cells settings.
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''#include <components/settings/categories/cells.hpp>
#include <components/settings/categories/shadows.hpp>''',
    '''#include <components/settings/categories/cells.hpp>
#include <components/settings/categories/shadows.hpp>
#include <components/settings/values.hpp>''',
)

replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''#include <array>
#include <mutex>''',
    '''#include <algorithm>
#include <array>
#include <mutex>''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        void setV36AsyncGpuProfiler(bool enabled) { _v36AsyncGpuProfiler = enabled; }''',
    '''        void setV36AsyncGpuProfiler(bool enabled) { _v36AsyncGpuProfiler = enabled; }
        void setV36FarCasterMinimumPixels(float pixels) { _v36FarCasterMinimumPixels = std::max(0.f, pixels); }
        float getV36FarCasterMinimumPixels() const { return _v36FarCasterMinimumPixels; }''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        bool                                    _v36AsyncGpuProfiler = false;''',
    '''        bool                                    _v36AsyncGpuProfiler = false;
        float                                   _v36FarCasterMinimumPixels = 0.f;''',
)
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV36AsyncGpuProfiler(Settings::cells().mV36AsyncGpuProfiler);''',
    '''        mShadowTechnique->setV36AsyncGpuProfiler(Settings::cells().mV36AsyncGpuProfiler);
        mShadowTechnique->setV36FarCasterMinimumPixels(Settings::cells().mV36FarCasterMinimumPixels);''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    // switch off small feature culling as this can cull out geometry that will still be large enough once perspective correction takes effect.
    _camera->setCullingMode(_camera->getCullingMode() & ~osg::CullSettings::SMALL_FEATURE_CULLING);''',
    '''    // Preserve upstream behavior unless the independently selected V3.6 far-caster experiment is active.
    // The threshold applies only to the farthest cascade; near and middle shadow quality is untouched.
    const float v36FarCasterMinimumPixels
        = vdd->getViewDependentShadowMap()->getV36FarCasterMinimumPixels();
    if (!debug && shadowMapCount > 1 && shadowMapIndex + 1 == shadowMapCount && v36FarCasterMinimumPixels > 0.f)
    {
        _camera->setCullingMode(_camera->getCullingMode() | osg::CullSettings::SMALL_FEATURE_CULLING);
        _camera->setSmallFeatureCullingPixelSize(v36FarCasterMinimumPixels);
    }
    else
        _camera->setCullingMode(_camera->getCullingMode() & ~osg::CullSettings::SMALL_FEATURE_CULLING);''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''        "max_reuse_texel_drift,far_width,far_height,far_resolution_divisor");''',
    '''        "max_reuse_texel_drift,far_width,far_height,far_resolution_divisor,shadow_distance,"
        "far_caster_min_pixels");''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''            << _v33FarCascadeResolutionDivisor;''',
    '''            << _v33FarCascadeResolutionDivisor << ',' << settings->getMaximumShadowMapDistance() << ','
            << _v36FarCasterMinimumPixels;''',
)

print("V3.6 far-cascade projected-size caster pruning source patch completed successfully.")
