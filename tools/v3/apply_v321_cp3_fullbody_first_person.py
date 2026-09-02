import os
import subprocess
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP3 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP3 patched {rel} ({count} match(es))")


# Register the default-off feature and a bounded camera-forward distance. The
# launcher environment override is resolved once by Camera construction;
# ordinary first person does not pay a per-frame settings or environment lookup.
replace_exact(
    "components/settings/categories/camera.hpp",
    '''        SettingValue<float> mFirstPersonFieldOfView{ mIndex, "Camera", "first person field of view",
            makeClampSanitizerFloat(1, 179) };
        SettingValue<bool> mReverseZ{ mIndex, "Camera", "reverse z" };''',
    '''        SettingValue<float> mFirstPersonFieldOfView{ mIndex, "Camera", "first person field of view",
            makeClampSanitizerFloat(1, 179) };
        SettingValue<bool> mV321FullBodyFirstPerson{ mIndex, "Camera", "v3.21 full body first person" };
        SettingValue<float> mV321FullBodyFirstPersonForwardOffset{ mIndex, "Camera",
            "v3.21 full body first person forward offset", makeClampSanitizerFloat(0, 20) };
        SettingValue<bool> mReverseZ{ mIndex, "Camera", "reverse z" };''',
)
replace_exact(
    "files/settings-default.cfg",
    '''first person field of view = 60.0

# Reverse the depth range, reduces z-fighting of distant objects and terrain''',
    '''first person field of view = 60.0

# V3.21 CP3: render the normal player body and equipment from the native first-person camera.
# The separate head and hair parts remain hidden. A small yaw-relative forward
# offset places the camera at the eyes/in front of the open top of the neck.
v3.21 full body first person = false
v3.21 full body first person forward offset = 10.0

# Reverse the depth range, reduces z-fighting of distant objects and terrain''',
)

# Add a distinct internal animation view. Camera::Mode::FirstPerson remains the
# public camera mode, preserving saves, scripts, input, camera positioning, and
# every existing isFirstPerson() caller.
replace_exact(
    "apps/openmw/mwrender/npcanimation.hpp",
    '''            VM_Normal,
            VM_FirstPerson,
            VM_HeadOnly''',
    '''            VM_Normal,
            VM_FirstPerson,
            VM_FirstPersonFullBody,
            VM_HeadOnly''',
)

replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''        bool viewChange = mViewMode == VM_FirstPerson || viewMode == VM_FirstPerson;''',
    '''        const auto isFirstPersonView = [](ViewMode mode) {
            return mode == VM_FirstPerson || mode == VM_FirstPersonFullBody;
        };
        bool viewChange = isFirstPersonView(mViewMode) || isFirstPersonView(viewMode);''',
)

replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        for (size_t i = 0; i < slotlistsize && mViewMode != VM_HeadOnly; i++)
        {
            MWWorld::ConstContainerStoreIterator store = inv.getSlot(slotlist[i].mSlot);

            removePartGroup(slotlist[i].mSlot);

            if (store == inv.end())
                continue;''',
    '''        const bool isFullBodyFirstPerson = mViewMode == VM_FirstPersonFullBody;
        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        for (size_t i = 0; i < slotlistsize && mViewMode != VM_HeadOnly; i++)
        {
            MWWorld::ConstContainerStoreIterator store = inv.getSlot(slotlist[i].mSlot);

            removePartGroup(slotlist[i].mSlot);

            // Keep face-obscuring equipment out of the camera while retaining
            // the normal third-person body-part path for every other slot.
            if (isFullBodyFirstPerson && slotlist[i].mSlot == MWWorld::InventoryStore::Slot_Helmet)
            {
                removeIndividualPart(ESM::PRT_Head);
                removeIndividualPart(ESM::PRT_Hair);
                continue;
            }

            if (store == inv.end())
                continue;''',
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''        if (mViewMode != VM_FirstPerson)
        {
            if (mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Head, -1, 1, mHeadModel);
            if (mPartPriorities[ESM::PRT_Hair] < 1 && mPartPriorities[ESM::PRT_Head] <= 1 && !mHairModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Hair, -1, 1, mHairModel);
        }''',
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
)
replace_exact(
    "apps/openmw/mwrender/npcanimation.cpp",
    '''        else if (mViewMode == VM_Normal)
        {
            WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());
        }''',
    '''        else if (mViewMode == VM_Normal || mViewMode == VM_FirstPersonFullBody)
        {
            WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());
        }''',
)

# Resolve the feature once per Camera instance. Invalid environment values fail
# back to the typed setting. Mode129 and all older modes leave this false.
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''#include <components/misc/mathutil.hpp>
#include <components/sceneutil/nodecallback.hpp>''',
    '''#include <components/misc/mathutil.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/settings/values.hpp>

#include <cstdlib>
#include <string_view>''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''namespace
{

    class UpdateRenderCameraCallback''',
    '''namespace
{

    // openmw-custom-v3.21-cp3-fullbody-first-person
    bool v321FullBodyFirstPersonEnabled()
    {
        const bool configured = Settings::camera().mV321FullBodyFirstPerson;
        const char* value = std::getenv("OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON");
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
    '''        Mode getMode() const { return mMode; }
        std::optional<Mode> getQueuedMode() const { return mQueuedMode; }''',
    '''        Mode getMode() const { return mMode; }
        bool isFullBodyFirstPerson() const
        {
            return mMode == Mode::FirstPerson && mV321FullBodyFirstPerson;
        }
        std::optional<Mode> getQueuedMode() const { return mQueuedMode; }''',
)
replace_exact(
    "apps/openmw/mwrender/camera.hpp",
    '''        bool mFirstPersonView;

        Mode mMode;''',
    '''        bool mFirstPersonView;
        const bool mV321FullBodyFirstPerson;
        const float mV321FullBodyFirstPersonForwardOffset;

        Mode mMode;''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''        , mFirstPersonView(true)
        , mMode(Mode::FirstPerson)''',
    '''        , mFirstPersonView(true)
        , mV321FullBodyFirstPerson(v321FullBodyFirstPersonEnabled())
        , mV321FullBodyFirstPersonForwardOffset(Settings::camera().mV321FullBodyFirstPersonForwardOffset)
        , mMode(Mode::FirstPerson)''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''        osg::Vec3d res = trackedPosition;
        osg::Vec2f horizontalOffset
            = Misc::rotateVec2f(osg::Vec2f(mFirstPersonOffset.x(), mFirstPersonOffset.y()), mYaw);
        res.x() += horizontalOffset.x();''',
    '''        osg::Vec3d res = trackedPosition;
        osg::Vec2f localOffset(mFirstPersonOffset.x(), mFirstPersonOffset.y());
        // Keep this offset horizontal/yaw-relative. Applying pitch would drive
        // the camera back into the torso precisely when the player looks down.
        if (mV321FullBodyFirstPerson)
            localOffset.y() += mV321FullBodyFirstPersonForwardOffset;
        osg::Vec2f horizontalOffset = Misc::rotateVec2f(localOffset, mYaw);
        res.x() += horizontalOffset.x();''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''        if (mMode == Mode::FirstPerson)
        {
            mAnimation->setViewMode(NpcAnimation::VM_FirstPerson);
            mTrackingNode = mAnimation->getNode("Camera");''',
    '''        if (mMode == Mode::FirstPerson)
        {
            mAnimation->setViewMode(mV321FullBodyFirstPerson ? NpcAnimation::VM_FirstPersonFullBody
                                                            : NpcAnimation::VM_FirstPerson);
            mTrackingNode = mAnimation->getNode("Camera");''',
)

# Keep the runtime log identity complete. The build manifest already carries
# the CP3 variant, but the engine identity line must identify it too.
replace_exact(
    "apps/openmw/engine.cpp",
    'openmw-custom-v3.21-cp2-fairness-dephasing',
    'openmw-custom-v3.21-cp2-fairness-dephasing / openmw-custom-v3.21-cp3-fullbody-first-person',
)

# Reanimation Lua can select the third-person animation path while the public
# camera remains FirstPerson. This is an additive read-only API.
replace_exact(
    "apps/openmw/mwlua/camerabindings.cpp",
    '''        api["getMode"] = [camera]() -> int { return static_cast<int>(camera->getMode()); };
        api["getQueuedMode"]''',
    '''        api["getMode"] = [camera]() -> int { return static_cast<int>(camera->getMode()); };
        api["isFullBodyFirstPerson"] = [camera]() { return camera->isFullBodyFirstPerson(); };
        api["getQueuedMode"]''',
)

# Mode130 is exactly accepted Mode129 plus CP3. Unified Test remains the normal
# benchmark entry point; City Frametime remains only its compatibility alias.
launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
launcher = launcher.replace(
    "$V321CP2Fairness = '0'\n$V320EngineLuaFastPaths = '0'",
    "$V321CP2Fairness = '0'\n$V321CP3FullBodyFirstPerson = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)
menu129 = "Write-Host '129 = V3.21 CP2 class-aware completion fairness/dephasing'"
if launcher.count(menu129) != 1:
    raise RuntimeError("V3.21 CP3 launcher lost Mode129 menu anchor")
launcher = launcher.replace(
    menu129,
    menu129 + "\nWrite-Host '130 = V3.21 CP3 true full-body first person (Mode129 + FBFP)'",
    1,
)
choice_line = next((line for line in launcher.splitlines() if "Enter a listed mode (1-127 or 129)" in line), None)
if not choice_line or ",'129'))" not in choice_line:
    raise RuntimeError("V3.21 CP3 launcher choice anchor drifted")
new_choice = choice_line.replace(
    "Enter a listed mode (1-127 or 129)", "Enter a listed mode (1-127 or 129-130)", 1
).replace(",'129'))", ",'129','130'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)
line129 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'129'"))
mode130_body = line129[line129.index("{") + 1 : line129.rindex("}")].strip()
if "v321-cp2-fairness-dephasing" not in mode130_body or "$V321CP2Fairness = '1'" not in mode130_body:
    raise RuntimeError("V3.21 CP3 Mode129 source body drifted")
mode130_body = mode130_body.replace(
    "v321-cp2-fairness-dephasing", "v321-cp3-fullbody-first-person", 1
)
mode130 = "        '130' { " + mode130_body + "; $V321CP3FullBodyFirstPerson = '1' }"
launcher = launcher.replace(line129 + "\n", line129 + "\n" + mode130 + "\n", 1)
manifest_anchor = '    "v321_cp2_fairness=$V321CP2Fairness",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.21 CP3 manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v321_cp3_fullbody_first_person=$V321CP3FullBodyFirstPerson",',
    1,
)
env_anchor = "    $env:OPENMW_V321_CP2_FAIRNESS = $V321CP2Fairness"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.21 CP3 environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON = $V321CP3FullBodyFirstPerson",
    1,
)
cleanup_anchor = "    Remove-Item Env:OPENMW_V321_CP2_FAIRNESS -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.21 CP3 cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON -ErrorAction SilentlyContinue\n"
    + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''


V3.21 CP3 — true full-body first person
=========================================

Mode 130 is Mode 129 plus a switchable full-body first-person view. The public
camera mode remains Camera::Mode::FirstPerson, so camera positioning, input,
saves, and existing first-person script checks retain their established
semantics. Internally, the player NpcAnimation selects VM_FirstPersonFullBody.

The full-body view uses the normal player skeleton, normal third-person body and
equipment parts, weapon controllers, the ordinary world field of view, and the
ordinary world-depth render path. It does not use Dot1st body parts, the native
first-person skeleton, the first-person-only FOV callback, the DepthClear render
bin, or the native first-person neck controller. Head, hair, and helmet parts are
suppressed in the owner view while the separate normal PRT_Neck part remains.
The camera is shifted 10 units forward relative to character yaw, placing the
viewpoint ahead of both the hidden head and the neck opening. The offset does not
follow pitch, so looking down cannot drive the camera into the torso. Switching
back to third person rebuilds the complete normal player normally.

The additive Lua API camera.isFullBodyFirstPerson() returns true only when the
public camera is FirstPerson and CP3 is active. Reanimation scripts can therefore
retain Rogue/native behavior for ordinary first person and use their third-person
animation path for the CP3 body without changing the public camera mode.

The feature defaults off in settings and Mode 129. Mode 130 enables it through a
process-local launcher environment override and retains Mode129 fairness tuning
unchanged. Use V3_Unified_Test.bat for normal CP3 validation. V3_City_Frametime.bat
remains a compatibility alias for the same City dataset.
'''
readme_path.write_text(readme, encoding="utf-8", newline="\n")

# Refresh the exact generated patch only after the complete CP3 layer.
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

# Fail closed on architecture, launcher, and disabled-path drift.
camera_hpp = (ROOT / "apps/openmw/mwrender/camera.hpp").read_text(encoding="utf-8")
camera_cpp = (ROOT / "apps/openmw/mwrender/camera.cpp").read_text(encoding="utf-8")
engine_cpp = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
npc_hpp = (ROOT / "apps/openmw/mwrender/npcanimation.hpp").read_text(encoding="utf-8")
npc_cpp = (ROOT / "apps/openmw/mwrender/npcanimation.cpp").read_text(encoding="utf-8")
lua_camera = (ROOT / "apps/openmw/mwlua/camerabindings.cpp").read_text(encoding="utf-8")
camera_settings = (ROOT / "components/settings/categories/camera.hpp").read_text(encoding="utf-8")
defaults = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")

for marker in (
    "VM_FirstPersonFullBody",
    "isFullBodyFirstPerson",
    "mV321FullBodyFirstPerson",
    "mV321FullBodyFirstPersonForwardOffset",
    "OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON",
    "openmw-custom-v3.21-cp3-fullbody-first-person",
):
    if marker not in camera_hpp + camera_cpp + engine_cpp + npc_hpp + npc_cpp + lua_camera:
        raise RuntimeError(f"V3.21 CP3 generated source missing marker: {marker}")
if "mV321FullBodyFirstPerson" not in camera_settings:
    raise RuntimeError("V3.21 CP3 setting is not registered")
if "mV321FullBodyFirstPersonForwardOffset" not in camera_settings:
    raise RuntimeError("V3.21 CP3 forward-offset setting is not registered")
if "v3.21 full body first person = false" not in defaults:
    raise RuntimeError("V3.21 CP3 setting is not default-off")
if "v3.21 full body first person forward offset = 10.0" not in defaults:
    raise RuntimeError("V3.21 CP3 forward-offset default drifted")
for marker in (
    "130 = V3.21 CP3 true full-body first person",
    "v321-cp3-fullbody-first-person",
    "v321_cp3_fullbody_first_person=$V321CP3FullBodyFirstPerson",
    "Enter a listed mode (1-127 or 129-130)",
):
    if marker not in launcher:
        raise RuntimeError(f"V3.21 CP3 launcher missing marker: {marker}")
line129 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'129'"))
line130 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'130'"))
if "$V321CP3FullBodyFirstPerson = '1'" in line129:
    raise RuntimeError("V3.21 CP3 contaminated Mode129")
if "$V321CP2Fairness = '1'" not in line130 or "$V321CP3FullBodyFirstPerson = '1'" not in line130:
    raise RuntimeError("V3.21 Mode130 did not preserve CP2 and enable CP3")
if "mViewMode == VM_FirstPersonFullBody" not in npc_cpp:
    raise RuntimeError("V3.21 CP3 body-part isolation is missing")
if "localOffset.y() += mV321FullBodyFirstPersonForwardOffset;" not in camera_cpp:
    raise RuntimeError("V3.21 CP3 yaw-relative camera-forward offset is missing")
if "PRT_Neck is a" not in npc_cpp:
    raise RuntimeError("V3.21 CP3 headless owner-view isolation is missing")
if "return mode == VM_FirstPerson || mode == VM_FirstPersonFullBody;" not in npc_cpp:
    raise RuntimeError("V3.21 CP3 first-person transition handling is incomplete")
if "mViewMode == VM_Normal || mViewMode == VM_FirstPersonFullBody" not in npc_cpp:
    raise RuntimeError("V3.21 CP3 normal weapon-controller path is missing")
if "if (mViewMode == VM_FirstPerson)" not in npc_cpp or '"DepthClear"' not in npc_cpp:
    raise RuntimeError("V3.21 CP3 changed or lost the native first-person render path")

print("V3.21 CP3 switchable true full-body first-person layer applied")
