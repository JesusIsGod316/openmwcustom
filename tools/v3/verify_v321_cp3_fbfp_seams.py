from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(rel: str, marker: str, expected: int = 1) -> None:
    text = (ROOT / rel).read_text(encoding="utf-8")
    count = text.count(marker)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} CP3 seam marker(s) {marker!r}, found {count}")


# Public camera API must remain the existing five-value contract. CP3 P0 adds
# only an internal NPC animation view mode and an additive Lua query.
require(
    "apps/openmw/mwrender/camera.hpp",
    "            FirstPerson = 1,\n            ThirdPerson = 2,\n            Vanity = 3,\n            Preview = 4",
)
require(
    "apps/openmw/mwlua/camerabindings.cpp",
    'lua.create_table_with("Static", CameraMode::Static, "FirstPerson", CameraMode::FirstPerson, "ThirdPerson",',
)

# Current public first-person camera maps directly to the native first-person
# NpcAnimation mode. CP3 will conditionally select VM_FirstPersonFullBody here.
require(
    "apps/openmw/mwrender/camera.cpp",
    "            mAnimation->setViewMode(NpcAnimation::VM_FirstPerson);",
)
require(
    "apps/openmw/mwrender/camera.cpp",
    '            mTrackingNode = mAnimation->getNode("Camera");',
)
require(
    "apps/openmw/mwrender/camera.cpp",
    '                mTrackingNode = mAnimation->getNode("Head");',
)

# NpcAnimation is the semantic split point: native FP currently selects .1st
# skeleton/body/rendering behavior. Full-body FP must be a separate internal mode.
require(
    "apps/openmw/mwrender/npcanimation.hpp",
    "            VM_Normal,\n            VM_FirstPerson,\n            VM_HeadOnly",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "        bool is1stPerson = mViewMode == VM_FirstPerson;",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "                base = Settings::models().mXbaseanim1st.get().value();",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "VFS::Path::toNormalized(getActorSkeleton(is1stPerson, isFemale, isBeast, isWerewolf))",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "            = getBodyParts(race, !mNpc->isMale(), mViewMode == VM_FirstPerson, isWerewolf);",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    '        const char* ext = (mViewMode == VM_FirstPerson) ? ".1st" : "";',
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "            mObjectRoot->setNodeMask(Mask_FirstPerson);",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "                RenderBin_FirstPerson, \"DepthClear\", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);",
)

# Normal skeleton routing must remain available as the FBFP path.
require(
    "apps/openmw/mwrender/actorutil.cpp",
    "    const std::string& getActorSkeleton(bool firstPerson, bool isFemale, bool isBeast, bool isWerewolf)",
)
require(
    "apps/openmw/mwrender/actorutil.cpp",
    "            else if (isBeast)\n                return Settings::models().mBaseanimkna.get().value();",
)
require(
    "apps/openmw/mwrender/actorutil.cpp",
    "                return Settings::models().mBaseanim.get().value();",
)

# Own-view-only head suppression uses a new child mask. Main scene culling can
# exclude it, while player-shadow traversal can explicitly include it.
require(
    "apps/openmw/mwrender/vismask.hpp",
    "        Mask_Groundcover = (1 << 20),",
)
require(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "        auto mask = ~(Mask_UpdateVisitor | Mask_SimpleWater);",
)
require(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "        if (Settings::shadows().mPlayerShadows)\n            shadowCastingTraversalMask |= Mask_Player;",
)
require(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '            mPlayerNode->setNodeMask(Mask_Player);',
)

# Helmet slot identity is propagated as addPartGroup's group argument, allowing
# all body-part pieces of a multi-part helmet to be masked without touching
# unrelated clothing/armor groups.
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "            { MWWorld::InventoryStore::Slot_Helmet, 0 },",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "    void NpcAnimation::addPartGroup(int group, int priority, const std::vector<ESM::PartReference>& parts,",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "                addOrReplaceIndividualPart(static_cast<ESM::PartReferenceType>(part.mPart), group, priority,",
)
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "    bool NpcAnimation::addOrReplaceIndividualPart(ESM::PartReferenceType type, int group, int priority,",
)

# View transitions rebuild the NPC, which is the reset boundary for FBFP-only
# per-part visibility masks.
require(
    "apps/openmw/mwrender/npcanimation.cpp",
    "        rebuild();\n        setRenderBin();",
)

print("V3.21 CP3 FBFP source seams verified; no engine behavior changed")
