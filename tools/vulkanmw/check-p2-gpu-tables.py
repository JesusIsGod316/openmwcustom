from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
publisher = (ROOT / "components/rendercore/updatebatch.hpp").read_text(encoding="utf-8")
tables = (ROOT / "components/render/backend/vsg/gpuscenetables.hpp").read_text(encoding="utf-8")
session = (ROOT / "components/render/backend/vsg/vsgsemanticsession.cpp").read_text(encoding="utf-8")
host = (ROOT / "components/render/backend/vsg/vsgruntimehost.cpp").read_text(encoding="utf-8")

required = [
    (publisher, "using AppliedObserver"),
    (publisher, "mAppliedObserver(batch, mWorld)"),
    (tables, "class GpuSceneTables final"),
    (tables, "RenderCore::ReparentInstance"),
    (tables, "DirtyRange"),
    (tables, "actorPlanSerial"),
    (host, "mGpuSceneTables.actorPlanSerial()"),
    ((ROOT / "components/render/backend/vsg/locallightplan.hpp").read_text(encoding="utf-8"),
     "buildLocalLightWorldPlan(\n        const GpuSceneTables& tables"),
    (host, "buildLocalLightWorldPlan(mGpuSceneTables, world)"),
    (tables, "forEachActorInstance"),
    ((ROOT / "components/render/backend/vsg/dynamicactorplan.hpp").read_text(encoding="utf-8"),
     "sceneTables->forEachActorInstance(appendActor)"),
    (tables, "lightSerial()"),
    (session, 'environmentFlag<"OPENMW_VK_GPU_SCENE_TABLES">()'),
    (session, "setAppliedObserver"),
    (host, "applyWorldUpdateBatch"),
    (host, "mGpuSceneTables.lightSerial()"),
    (host, "plan.sourceSerial = lightSerial"),
    (host, "P2 GPU scene tables missed or rejected an authoritative RenderWorld delta"),
]
for text, needle in required:
    if needle not in text:
        raise SystemExit(f"missing P2 GPU-table contract: {needle}")
print("VulkanMW P2 direct-delta GPU table source contract: PASS")
