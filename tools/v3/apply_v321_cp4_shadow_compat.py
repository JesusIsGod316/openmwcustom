import os
import subprocess
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP4 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP4 patched {rel} ({count} match(es))")


# Geometry on this mask is absent from the primary owner camera and gameplay
# ray tests, but remains available to shadow and secondary-view cameras.
replace_exact(
    "apps/openmw/mwrender/vismask.hpp",
    '''        Mask_Groundcover = (1 << 20),''',
    '''        Mask_Groundcover = (1 << 20),

        // Player geometry hidden only from the primary owner camera. Shadow,
        // reflection and refraction traversals may explicitly include it.
        Mask_PlayerSecondaryView = (1 << 21),''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        if (Settings::shadows().mPlayerShadows)
            shadowCastingTraversalMask |= Mask_Player;''',
    '''        if (Settings::shadows().mPlayerShadows)
            shadowCastingTraversalMask |= Mask_Player | Mask_PlayerSecondaryView;''',
)
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        auto mask = ~(Mask_UpdateVisitor | Mask_SimpleWater);''',
    '''        auto mask = ~(Mask_UpdateVisitor | Mask_SimpleWater | Mask_PlayerSecondaryView);''',
)
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        mask &= ~(Mask_RenderToTexture | Mask_Sky | Mask_Debug | Mask_Effect | Mask_Water | Mask_SimpleWater
            | Mask_Groundcover);''',
    '''        mask &= ~(Mask_RenderToTexture | Mask_Sky | Mask_Debug | Mask_Effect | Mask_Water | Mask_SimpleWater
            | Mask_Groundcover | Mask_PlayerSecondaryView);''',
)
replace_exact(
    "apps/openmw/mwrender/water.cpp",
    '''            | Mask_Terrain | Mask_Actor | Mask_ParticleSystem | Mask_Sky | Mask_Sun | Mask_Player | Mask_Lighting
            | Mask_Groundcover;''',
    '''            | Mask_Terrain | Mask_Actor | Mask_ParticleSystem | Mask_Sky | Mask_Sun | Mask_Player
            | Mask_PlayerSecondaryView | Mask_Lighting | Mask_Groundcover;''',
)
replace_exact(
    "apps/openmw/mwrender/water.cpp",
    '''                extraMask |= Mask_Player | Mask_Actor;''',
    '''                extraMask |= Mask_Player | Mask_PlayerSecondaryView | Mask_Actor;''',
)

# Mode130 remains the accepted CP3 behavior. Mode131 asks NpcAnimation to retain
# head-provided equipment solely on the secondary-view mask.
replace_exact(
    "components/settings/categories/camera.hpp",
    '''        SettingValue<bool> mV321FullBodyFirstPerson{ mIndex, "Camera", "v3.21 full body first person" };
        SettingValue<float> mV321FullBodyFirstPersonForwardOffset{ mIndex, "Camera",
            "v3.21 full body first person forward offset", makeClampSanitizerFloat(0, 20) };''',
    '''        SettingValue<bool> mV321FullBodyFirstPerson{ mIndex, "Camera", "v3.21 full body first person" };
        SettingValue<bool> mV321FullBodyFirstPersonShadowCompat{ mIndex, "Camera",
            "v3.21 full body first person shadow compatibility" };
        SettingValue<float> mV321FullBodyFirstPersonForwardOffset{ mIndex, "Camera",
            "v3.21 full body first person forward offset", makeClampSanitizerFloat(0, 20) };''',
)
replace_exact(
    "files/settings-default.cfg",
    '''v3.21 full body first person = false
v3.21 full body first person forward offset = 10.0''',
    '''v3.21 full body first person = false
v3.21 full body first person shadow compatibility = false
v3.21 full body first person forward offset = 10.0''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''    class UpdateRenderCameraCallback''',
    '''    bool v321FullBodyFirstPersonShadowCompatEnabled()
    {
        const bool configured = Settings::camera().mV321FullBodyFirstPersonShadowCompat;
        const char* value = std::getenv("OPENMW_V321_CP4_SHADOW_COMPAT");
        if (value == nullptr || *value == '\\0')
            return configured;
        const std::string_view parsed(value);
        if (parsed == "1" || parsed == "true")
            return true;
        if (parsed == "0" || parsed == "false")
            return false;
        return configured;
    }

    class UpdateRenderCameraCallback''',
)
replace_exact(
    "apps/openmw/mwrender/camera.hpp",
    '''        bool isFullBodyFirstPerson() const
        {
            return mMode == Mode::FirstPerson && mV321FullBodyFirstPerson;
        }
        std::optional<Mode> getQueuedMode() const { return mQueuedMode; }''',
    '''        bool isFullBodyFirstPerson() const
        {
            return mMode == Mode::FirstPerson && mV321FullBodyFirstPerson;
        }
        Mode getAnimationMode() const
        {
            return isFullBodyFirstPerson() ? Mode::ThirdPerson : mMode;
        }
        std::optional<Mode> getQueuedMode() const { return mQueuedMode; }''',
)
replace_exact(
    "apps/openmw/mwrender/camera.hpp",
    '''        const bool mV321FullBodyFirstPerson;
        const float mV321FullBodyFirstPersonForwardOffset;''',
    '''        const bool mV321FullBodyFirstPerson;
        const bool mV321FullBodyFirstPersonShadowCompat;
        const float mV321FullBodyFirstPersonForwardOffset;''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''        , mV321FullBodyFirstPerson(v321FullBodyFirstPersonEnabled())
        , mV321FullBodyFirstPersonForwardOffset(Settings::camera().mV321FullBodyFirstPersonForwardOffset)''',
    '''        , mV321FullBodyFirstPerson(v321FullBodyFirstPersonEnabled())
        , mV321FullBodyFirstPersonShadowCompat(v321FullBodyFirstPersonShadowCompatEnabled())
        , mV321FullBodyFirstPersonForwardOffset(Settings::camera().mV321FullBodyFirstPersonForwardOffset)''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''            mAnimation->setViewMode(mV321FullBodyFirstPerson ? NpcAnimation::VM_FirstPersonFullBody
                                                            : NpcAnimation::VM_FirstPerson);''',
    '''            mAnimation->setViewMode(mV321FullBodyFirstPerson ? NpcAnimation::VM_FirstPersonFullBody
                                                            : NpcAnimation::VM_FirstPerson,
                mV321FullBodyFirstPerson && mV321FullBodyFirstPersonShadowCompat);''',
)

replace_exact(
    "apps/openmw/mwrender/npcanimation.hpp",
    '''        ViewMode mViewMode;
        bool mShowWeapons;''',
    '''        ViewMode mViewMode;
        bool mV321FullBodyShadowCompat = false;
        bool mShowWeapons;''',
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.hpp",
    '''        void setViewMode(ViewMode viewMode);''',
    '''        void setViewMode(ViewMode viewMode, bool fullBodyShadowCompat = false);''',
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''    void NpcAnimation::setViewMode(NpcAnimation::ViewMode viewMode)
    {
        assert(viewMode != VM_HeadOnly);
        if (mViewMode == viewMode)
            return;''',
    '''    void NpcAnimation::setViewMode(NpcAnimation::ViewMode viewMode, bool fullBodyShadowCompat)
    {
        assert(viewMode != VM_HeadOnly);
        fullBodyShadowCompat = viewMode == VM_FirstPersonFullBody && fullBodyShadowCompat;
        if (mViewMode == viewMode && mV321FullBodyShadowCompat == fullBodyShadowCompat)
            return;''',
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''        bool viewChange = isFirstPersonView(mViewMode) || isFirstPersonView(viewMode);
        mViewMode = viewMode;''',
    '''        bool viewChange = isFirstPersonView(mViewMode) || isFirstPersonView(viewMode);
        mViewMode = viewMode;
        mV321FullBodyShadowCompat = fullBodyShadowCompat;''',
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''            if (isFullBodyFirstPerson && slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Helmet)
            {
                removeIndividualPart(ESM::PRT_Head);
                removeIndividualPart(ESM::PRT_Hair);
                continue;
            }''',
    '''            if (isFullBodyFirstPerson && !mV321FullBodyShadowCompat
                && slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Helmet)
            {
                removeIndividualPart(ESM::PRT_Head);
                removeIndividualPart(ESM::PRT_Hair);
                continue;
            }''',
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''        if (isFullBodyFirstPerson)
        {
            // Equipment groups can reference head or hair from slots other than
            // Helmet. Keep both absent from the owner view. PRT_Neck is a
            // separate normal body part and remains attached below.
            removeIndividualPart(ESM::PRT_Head);
            removeIndividualPart(ESM::PRT_Hair);
        }
        else if (mViewMode != VM_FirstPerson)
        {
            if (mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Head, -1, 1, mHeadModel);
            if (mPartPriorities[ESM::PRT_Hair] < 1 && mPartPriorities[ESM::PRT_Head] <= 1 && !mHairModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Hair, -1, 1, mHairModel);
        }''',
    '''        if (isFullBodyFirstPerson && !mV321FullBodyShadowCompat)
        {
            // CP3 control: head-provided geometry is absent from every traversal.
            removeIndividualPart(ESM::PRT_Head);
            removeIndividualPart(ESM::PRT_Hair);
        }
        else if (mViewMode != VM_FirstPerson)
        {
            if (mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Head, -1, 1, mHeadModel);
            if (mPartPriorities[ESM::PRT_Hair] < 1 && mPartPriorities[ESM::PRT_Head] <= 1 && !mHairModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Hair, -1, 1, mHairModel);
        }

        if (isFullBodyFirstPerson && mV321FullBodyShadowCompat)
        {
            // CP4: retain the animated head/hair and every part supplied by a
            // helmet for secondary views without exposing them to the owner
            // camera. PRT_Neck is a separate normal body part and remains
            // attached. Checking part ownership also covers modded helmets
            // that use neck or other nonstandard body-part records.
            for (size_t i = 0; i < ESM::PRT_Count; ++i)
            {
                const bool ownerHidden = i == ESM::PRT_Head || i == ESM::PRT_Hair
                    || mPartslots[i] == MWWorld::InventoryStore::Slot_Helmet;
                if (ownerHidden && mObjectParts[i])
                    mObjectParts[i]->getNode()->setNodeMask(Mask_PlayerSecondaryView);
            }
        }''',
)

# Add a compatibility-oriented mode query while preserving the established
# public camera mode. Reanimation-style scripts can use their normal third-
# person branch without interpreting the CP3-specific boolean themselves.
replace_exact(
    "apps/openmw/mwlua/camerabindings.cpp",
    '''        api["isFullBodyFirstPerson"] = [camera]() { return camera->isFullBodyFirstPerson(); };
        api["getQueuedMode"]''',
    '''        api["isFullBodyFirstPerson"] = [camera]() { return camera->isFullBodyFirstPerson(); };
        api["getAnimationMode"] = [camera]() -> int { return static_cast<int>(camera->getAnimationMode()); };
        api["getQueuedMode"]''',
)
replace_exact(
    "files/lua_api/openmw/camera.lua",
    '''-- Return the current @{openmw.camera#MODE}.
-- @function [parent=#camera] getMode
-- @return #Mode

---
-- Return the mode the camera will switch to after the end of the current animation. Can be nil.''',
    '''-- Return the current @{openmw.camera#MODE}. Full-body first person remains FirstPerson.
-- @function [parent=#camera] getMode
-- @return #Mode

---
-- Return true when the engine is using the full-body first-person owner view.
-- @function [parent=#camera] isFullBodyFirstPerson
-- @return #boolean

---
-- Return the recommended animation branch mode. This returns ThirdPerson for
-- full-body first person and otherwise matches `getMode()`.
-- @function [parent=#camera] getAnimationMode
-- @return #Mode

---
-- Return the mode the camera will switch to after the end of the current animation. Can be nil.''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    'openmw-custom-v3.21-cp3-fullbody-first-person',
    'openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat',
)

# Mode131 is exact Mode130 plus the CP4 shadow compatibility switch.
launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
launcher = launcher.replace(
    "$V321CP3FullBodyFirstPerson = '0'\n$V320EngineLuaFastPaths = '0'",
    "$V321CP3FullBodyFirstPerson = '0'\n$V321CP4ShadowCompat = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)
menu130 = "Write-Host '130 = V3.21 CP3 true full-body first person (Mode129 + FBFP)'"
if launcher.count(menu130) != 1:
    raise RuntimeError("V3.21 CP4 launcher lost Mode130 menu anchor")
launcher = launcher.replace(
    menu130,
    menu130 + "\nWrite-Host '131 = V3.21 CP4 full-body shadow and animation compatibility'",
    1,
)
choice_line = next((line for line in launcher.splitlines() if "Enter a listed mode (1-127 or 129-130)" in line), None)
if not choice_line or ",'130'))" not in choice_line:
    raise RuntimeError("V3.21 CP4 launcher choice anchor drifted")
new_choice = choice_line.replace(
    "Enter a listed mode (1-127 or 129-130)", "Enter a listed mode (1-127 or 129-131)", 1
).replace(",'130'))", ",'130','131'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)
line130 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'130'"))
mode131_body = line130[line130.index("{") + 1 : line130.rindex("}")].strip()
if "$V321CP3FullBodyFirstPerson = '1'" not in mode131_body:
    raise RuntimeError("V3.21 CP4 Mode130 source body drifted")
mode131_body = mode131_body.replace(
    "v321-cp3-fullbody-first-person", "v321-cp4-shadow-compat", 1
)
mode131 = "        '131' { " + mode131_body + "; $V321CP4ShadowCompat = '1' }"
launcher = launcher.replace(line130 + "\n", line130 + "\n" + mode131 + "\n", 1)
manifest_anchor = '    "v321_cp3_fullbody_first_person=$V321CP3FullBodyFirstPerson",'
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v321_cp4_shadow_compat=$V321CP4ShadowCompat",',
    1,
)
env_anchor = "    $env:OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON = $V321CP3FullBodyFirstPerson"
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V321_CP4_SHADOW_COMPAT = $V321CP4ShadowCompat",
    1,
)
cleanup_anchor = "    Remove-Item Env:OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON -ErrorAction SilentlyContinue"
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V321_CP4_SHADOW_COMPAT -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''


V3.21 CP4 — full-body shadow and animation compatibility
=========================================================

Mode 130 remains the accepted CP3 owner-view control. Mode 131 is exact Mode130
plus CP4 shadow compatibility. CP4 retains the real animated head, hair and
helmet parts on Mask_PlayerSecondaryView. The primary owner camera and gameplay
ray tests omit this mask, so the accepted head-free view is unchanged. Player
shadow maps and water reflection/refraction cameras explicitly include it,
allowing a complete equipped silhouette without a second actor or skeleton.

Lua keeps camera.getMode() backward compatible: full-body first person remains
MODE.FirstPerson. camera.isFullBodyFirstPerson() remains the exact feature test.
The new camera.getAnimationMode() compatibility helper returns MODE.ThirdPerson
for full-body first person and otherwise matches getMode(). Reanimation-style
mods can therefore select their existing third-person/full-body branch with a
single mode query while retaining a simple fallback on older OpenMW builds.
'''
readme_path.write_text(readme, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

# Fail closed on owner-view isolation, secondary-view coverage and API drift.
vismask = (ROOT / "apps/openmw/mwrender/vismask.hpp").read_text(encoding="utf-8")
rendering = (ROOT / "apps/openmw/mwrender/renderingmanager.cpp").read_text(encoding="utf-8")
water = (ROOT / "apps/openmw/mwrender/water.cpp").read_text(encoding="utf-8")
npc_hpp = (ROOT / "apps/openmw/mwrender/npcanimation.hpp").read_text(encoding="utf-8")
npc_cpp = (ROOT / "apps/openmw/mwrender/npcanimation.cpp").read_text(encoding="utf-8")
camera_hpp = (ROOT / "apps/openmw/mwrender/camera.hpp").read_text(encoding="utf-8")
camera_cpp = (ROOT / "apps/openmw/mwrender/camera.cpp").read_text(encoding="utf-8")
lua_bindings = (ROOT / "apps/openmw/mwlua/camerabindings.cpp").read_text(encoding="utf-8")
lua_docs = (ROOT / "files/lua_api/openmw/camera.lua").read_text(encoding="utf-8")
defaults = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")

if "Mask_PlayerSecondaryView = (1 << 21)" not in vismask:
    raise RuntimeError("V3.21 CP4 secondary-view mask missing")
for marker in (
    "Mask_Player | Mask_PlayerSecondaryView",
    "Mask_SimpleWater | Mask_PlayerSecondaryView",
    "Mask_Groundcover | Mask_PlayerSecondaryView",
):
    if marker not in rendering:
        raise RuntimeError(f"V3.21 CP4 renderer mask missing: {marker}")
if water.count("Mask_PlayerSecondaryView") != 2:
    raise RuntimeError("V3.21 CP4 water secondary-view coverage drifted")
for marker in (
    "mV321FullBodyShadowCompat",
    "!mV321FullBodyShadowCompat",
    "setNodeMask(Mask_PlayerSecondaryView)",
):
    if marker not in npc_hpp + npc_cpp:
        raise RuntimeError(f"V3.21 CP4 NPC shadow isolation missing: {marker}")
if "getAnimationMode()" not in camera_hpp or "getAnimationMode" not in lua_bindings or "getAnimationMode" not in lua_docs:
    raise RuntimeError("V3.21 CP4 animation compatibility API incomplete")
if "OPENMW_V321_CP4_SHADOW_COMPAT" not in camera_cpp + launcher:
    raise RuntimeError("V3.21 CP4 runtime switch missing")
if "v3.21 full body first person shadow compatibility = false" not in defaults:
    raise RuntimeError("V3.21 CP4 shadow compatibility is not default-off")
line130 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'130'"))
line131 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'131'"))
if "$V321CP4ShadowCompat = '1'" in line130:
    raise RuntimeError("V3.21 CP4 contaminated Mode130")
if "$V321CP3FullBodyFirstPerson = '1'" not in line131 or "$V321CP4ShadowCompat = '1'" not in line131:
    raise RuntimeError("V3.21 Mode131 did not preserve CP3 and enable CP4")
if "openmw-custom-v3.21-cp4-shadow-compat" not in (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8"):
    raise RuntimeError("V3.21 CP4 engine identity missing")

print("V3.21 CP4 shadow and animation compatibility layer applied")
