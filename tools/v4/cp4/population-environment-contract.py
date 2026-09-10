#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"CP4C/CP4D contract missing from {path}: {needle}")


require(
    "components/rendercore/records.hpp",
    "struct StaticPopulationPayload",
    "std::vector<ModelPopulationRecord> groups",
    "std::shared_ptr<const StaticPopulationPayload> population",
)
require(
    "components/rendercore/staticpopulationproducer.hpp",
    "class StaticPopulationProducer final",
    'RenderWorldUpdateBatch batch(mWorld.epoch(), sequence, "static-populations")',
    "std::map<std::string, StaticPopulationInstanceSource, std::less<>> instances",
)
require(
    "components/render/backend/vsg/staticpopulationresidency.hpp",
    "struct StaticPopulationIdentity",
    "staticPopulationPlanCurrent(world, plan)",
    "FrameRetirementQueue<Object> mRetirements",
)
require(
    "components/render/backend/vsg/vsgruntimehost.cpp",
    "mStaticPopulationResidency.prepare(world, worldPlan)",
    "incremental VSG population compilation failed before scene publication",
    "mStaticPopulationResidency.markSubmitted(frame.frameId())",
    "vsg::HardShadows::create(options.shadows.cascadeCount)",
    "shadowSettingsOverride[mSunLight]",
    "maskedNode(placementMask(castsShadow)",
    "populationWithinMaximumDistance(world, plan, mainView.current.worldPosition)",
    "environment.skyEnabled ? environment.skyColor : environment.fogColor",
)
require(
    "components/render/backend/vsg/staticassetrealizer.cpp",
    '"vsg_Translation", VK_VERTEX_INPUT_RATE_INSTANCE',
    'assignArray(arrays, "vsg_Rotation", VK_VERTEX_INPUT_RATE_INSTANCE',
    'assignArray(arrays, "vsg_Scale", VK_VERTEX_INPUT_RATE_INSTANCE',
)
require(
    "apps/openmw/mwrender/v4scenerenderlifecycle.cpp",
    "mSession->populations().upsert",
    "mSession->populations().remove",
    "mSession->cells().upsertStaticInstance",
)
require(
    "apps/openmw/mwrender/groundcover.cpp",
    "Groundcover::InstanceMap Groundcover::collectInstances",
    "mGroundcoverStore.initCell",
    "DensityCalculator calculator(mDensity)",
)
require(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    "groundcover->collectInstances(1.0f, center)",
    "ensureModelPublished(*mSession, mVfs, modelPath)",
    "mSession->populations().removeCell",
)
require(
    "components/rendercore/framerenderstate.hpp",
    "Color skyColor",
    "float precipitationIntensity",
    "glm::vec3 windDirection",
    "bool shadowsEnabled",
)
require(
    "components/render/backend/vsg/openmwviewdependentstate.cpp",
    "environment.cloudBlendFactor",
    "environment.windDirection.x",
    "environment.precipitationEnabled",
    "environment.shadowsEnabled",
)
require(
    "apps/openmw/mwrender/v4semanticsource.cpp",
    "sky->getCloudBlendFactor()",
    "sky->getPrecipitationAlpha()",
    "sky->getStormDirection()",
    "result.skyEnabled = sky->isEnabled()",
)

print("CP4C/CP4D population and environment contracts: PASS")
