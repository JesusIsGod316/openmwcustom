import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.5 safety match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.5 safety patched {rel} ({count} match(es))")


# Groundcover owns an osg::ref_ptr<OcclusionCuller>; keep the type complete in the implementation
# unit so assignment/destruction never depends on transitive includes.
replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''#include "occlusionculling.hpp"

#include <span>''',
    '''#include "occlusionculling.hpp"

#include <components/sceneutil/occlusionculling.hpp>

#include <span>''',
)

# V3.5 deliberately allows the existing far-cascade reuse path while actor/player shadows
# are enabled, but dynamic caster motion is not represented in the camera-drift test. Bound
# that experiment to interval 2 so a moving dynamic caster can be stale for at most one
# rendered frame. Larger intervals remain available only when dynamic-caster reuse is off.
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV33FarCascadeReuse(static_cast<unsigned>(settings.mV33FarCascadeUpdateInterval),
            settings.mV33FarCascadeMaxTexelDrift,
            (settings.mActorShadows || settings.mPlayerShadows) && !settings.mV35AllowDynamicFarCascadeReuse);''',
    '''        const unsigned int v35ConfiguredFarInterval
            = static_cast<unsigned int>(settings.mV33FarCascadeUpdateInterval);
        const unsigned int v35EffectiveFarInterval
            = settings.mV35AllowDynamicFarCascadeReuse && v35ConfiguredFarInterval > 2u
            ? 2u
            : v35ConfiguredFarInterval;
        mShadowTechnique->setV33FarCascadeReuse(v35EffectiveFarInterval, settings.mV33FarCascadeMaxTexelDrift,
            (settings.mActorShadows || settings.mPlayerShadows) && !settings.mV35AllowDynamicFarCascadeReuse);''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# V3.5 permits the existing bounded far-cascade reuse path when actor/player shadows are enabled.
# When false, V3.3 behavior is preserved and actor/player shadows force interval 1.
v3.5 allow dynamic far cascade reuse = false''',
    '''# V3.5 permits the existing bounded far-cascade reuse path when actor/player shadows are enabled.
# Dynamic-caster reuse is safety-clamped to interval 2, so the cached far map can be at most one rendered frame old.
# When false, V3.3 behavior is preserved and actor/player shadows force interval 1.
v3.5 allow dynamic far cascade reuse = false''',
)

# Make the launcher wording describe the actual safety contract rather than implying a
# static/dynamic split that does not yet exist.
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    "Write-Host ' 20 = V3.5 dynamic far reuse + divisor 4 (far cascade interval 2)'",
    "Write-Host ' 20 = V3.5 bounded one-frame far reuse + divisor 4 (actor/player shadows stay enabled)'",
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '20' { $Experiment = 'v35-dynamic-far-reuse'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }''',
    '''    '20' { $Experiment = 'v35-bounded-far-reuse'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }''',
)

print("V3.5 safety pass completed successfully.")
