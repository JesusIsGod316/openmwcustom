import os
import subprocess
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP4 locomotion match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP4 locomotion patched {rel} ({count} match(es))")


# Preserve CP4's public FirstPerson camera semantics, but add a separate
# compatibility switch for a very narrow runtime safety fallback. Legacy Lua
# animation mods can replace a full-body locomotion group with a rootless
# first-person group while FBFP is active. The visual replacement should keep
# playing; it must not be allowed to zero physical player movement.
replace_exact(
    "components/settings/categories/camera.hpp",
    '''        SettingValue<bool> mV321FullBodyFirstPersonShadowCompat{ mIndex, "Camera",
            "v3.21 full body first person shadow compatibility" };
        SettingValue<float> mV321FullBodyFirstPersonForwardOffset{ mIndex, "Camera",''',
    '''        SettingValue<bool> mV321FullBodyFirstPersonShadowCompat{ mIndex, "Camera",
            "v3.21 full body first person shadow compatibility" };
        SettingValue<bool> mV321FullBodyFirstPersonLocomotionCompat{ mIndex, "Camera",
            "v3.21 full body first person locomotion compatibility" };
        SettingValue<float> mV321FullBodyFirstPersonForwardOffset{ mIndex, "Camera",''',
)
replace_exact(
    "files/settings-default.cfg",
    '''v3.21 full body first person = false
v3.21 full body first person shadow compatibility = false
v3.21 full body first person forward offset = 10.0''',
    '''v3.21 full body first person = false
v3.21 full body first person shadow compatibility = false
v3.21 full body first person locomotion compatibility = false
v3.21 full body first person forward offset = 10.0''',
)

replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''    class UpdateRenderCameraCallback''',
    '''    bool v321FullBodyFirstPersonLocomotionCompatEnabled()
    {
        const bool configured = Settings::camera().mV321FullBodyFirstPersonLocomotionCompat;
        const char* value = std::getenv("OPENMW_V321_CP4_LOCOMOTION_COMPAT");
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
    '''        Mode getAnimationMode() const
        {
            return isFullBodyFirstPerson() ? Mode::ThirdPerson : mMode;
        }
        std::optional<Mode> getQueuedMode() const { return mQueuedMode; }''',
    '''        Mode getAnimationMode() const
        {
            return isFullBodyFirstPerson() ? Mode::ThirdPerson : mMode;
        }
        bool isFullBodyFirstPersonLocomotionCompat() const
        {
            return isFullBodyFirstPerson() && mV321FullBodyFirstPersonLocomotionCompat;
        }
        std::optional<Mode> getQueuedMode() const { return mQueuedMode; }''',
)
replace_exact(
    "apps/openmw/mwrender/camera.hpp",
    '''        const bool mV321FullBodyFirstPerson;
        const bool mV321FullBodyFirstPersonShadowCompat;
        const float mV321FullBodyFirstPersonForwardOffset;''',
    '''        const bool mV321FullBodyFirstPerson;
        const bool mV321FullBodyFirstPersonShadowCompat;
        const bool mV321FullBodyFirstPersonLocomotionCompat;
        const float mV321FullBodyFirstPersonForwardOffset;''',
)
replace_exact(
    "apps/openmw/mwrender/camera.cpp",
    '''        , mV321FullBodyFirstPerson(v321FullBodyFirstPersonEnabled())
        , mV321FullBodyFirstPersonShadowCompat(v321FullBodyFirstPersonShadowCompatEnabled())
        , mV321FullBodyFirstPersonForwardOffset(Settings::camera().mV321FullBodyFirstPersonForwardOffset)''',
    '''        , mV321FullBodyFirstPerson(v321FullBodyFirstPersonEnabled())
        , mV321FullBodyFirstPersonShadowCompat(v321FullBodyFirstPersonShadowCompatEnabled())
        , mV321FullBodyFirstPersonLocomotionCompat(v321FullBodyFirstPersonLocomotionCompatEnabled())
        , mV321FullBodyFirstPersonForwardOffset(Settings::camera().mV321FullBodyFirstPersonForwardOffset)''',
)

# CharacterController still calculates the intended player velocity from input.
# Lua animation handlers may then replace the visual locomotion group. In FBFP,
# if such a replacement yields exactly zero root displacement, retain the
# already-calculated movement vector instead of replacing it with zero. The
# replacement animation continues to run, so leg/torso animation is preserved.
replace_exact(
    "apps/openmw/mwmechanics/character.cpp",
    '''#include "../mwrender/animation.hpp"''',
    '''#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"''',
)
replace_exact(
    "apps/openmw/mwmechanics/character.cpp",
    '''        osg::Vec3f movementFromAnimation
            = mAnimation->runAnimation(mSkipAnim && !isScriptedAnimPlaying() ? 0.f : duration);

        if (mPtr.getClass().isActor() && !isScriptedAnimPlaying())
        {
            if (isMovementAnimationControlled())
            {
                if (duration != 0.f && movementFromAnimation != osg::Vec3f())''',
    '''        osg::Vec3f movementFromAnimation
            = mAnimation->runAnimation(mSkipAnim && !isScriptedAnimPlaying() ? 0.f : duration);

        const bool v321FullBodyLuaLocomotionFallback = isPlayer && mLuaAnimations && !mSkipAnim
            && world->getCamera()->isFullBodyFirstPersonLocomotionCompat() && mMovementState != CharState_None
            && movement != osg::Vec3f() && movementFromAnimation == osg::Vec3f();

        if (mPtr.getClass().isActor() && !isScriptedAnimPlaying())
        {
            if (isMovementAnimationControlled())
            {
                if (duration != 0.f && movementFromAnimation != osg::Vec3f())''',
)
replace_exact(
    "apps/openmw/mwmechanics/character.cpp",
    '''                else
                {
                    movement = osg::Vec3f();
                }
            }
            else if (mSkipAnim)''',
    '''                else if (!v321FullBodyLuaLocomotionFallback)
                {
                    movement = osg::Vec3f();
                }
            }
            else if (mSkipAnim)''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    'openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat',
    'openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat / openmw-custom-v3.21-cp4-locomotion-compat',
)

# Mode132 is exact Mode131 plus the locomotion compatibility guard.
launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
launcher = launcher.replace(
    "$V321CP4ShadowCompat = '0'\n$V320EngineLuaFastPaths = '0'",
    "$V321CP4ShadowCompat = '0'\n$V321CP4LocomotionCompat = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)
menu131 = "Write-Host '131 = V3.21 CP4 full-body shadow and animation compatibility'"
if launcher.count(menu131) != 1:
    raise RuntimeError("V3.21 CP4 locomotion launcher lost Mode131 menu anchor")
launcher = launcher.replace(
    menu131,
    menu131 + "\nWrite-Host '132 = V3.21 CP4 full-body locomotion compatibility guard'",
    1,
)
choice_line = next((line for line in launcher.splitlines() if "Enter a listed mode (1-127 or 129-131)" in line), None)
if not choice_line or ",'131'))" not in choice_line:
    raise RuntimeError("V3.21 CP4 locomotion launcher choice anchor drifted")
new_choice = choice_line.replace(
    "Enter a listed mode (1-127 or 129-131)", "Enter a listed mode (1-127 or 129-132)", 1
).replace(",'131'))", ",'131','132'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)
line131 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'131'"))
mode132_body = line131[line131.index("{") + 1 : line131.rindex("}")].strip()
if "$V321CP3FullBodyFirstPerson = '1'" not in mode132_body or "$V321CP4ShadowCompat = '1'" not in mode132_body:
    raise RuntimeError("V3.21 CP4 Mode131 source body drifted")
mode132_body = mode132_body.replace(
    "v321-cp4-shadow-compat", "v321-cp4-locomotion-compat", 1
)
mode132 = "        '132' { " + mode132_body + "; $V321CP4LocomotionCompat = '1' }"
launcher = launcher.replace(line131 + "\n", line131 + "\n" + mode132 + "\n", 1)
manifest_anchor = '    "v321_cp4_shadow_compat=$V321CP4ShadowCompat",'
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v321_cp4_locomotion_compat=$V321CP4LocomotionCompat",',
    1,
)
env_anchor = "    $env:OPENMW_V321_CP4_SHADOW_COMPAT = $V321CP4ShadowCompat"
launcher = launcher.replace(
    env_anchor,
    env_anchor + "\n    $env:OPENMW_V321_CP4_LOCOMOTION_COMPAT = $V321CP4LocomotionCompat",
    1,
)
cleanup_anchor = "    Remove-Item Env:OPENMW_V321_CP4_SHADOW_COMPAT -ErrorAction SilentlyContinue"
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V321_CP4_LOCOMOTION_COMPAT -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''


V3.21 CP4 follow-up — full-body locomotion compatibility
=========================================================

Mode 131 remains the exact CP4 shadow/animation-API control. Mode 132 is exact
Mode131 plus a narrow player locomotion guard for full-body first person.

Some legacy Lua animation mods use camera.getMode()==MODE.FirstPerson to select
rootless first-person locomotion replacements even though the V3.21 FBFP owner
view renders the normal full body. CharacterController correctly computes the
player's intended movement from input before animation evaluation, but vanilla
root-motion semantics can then replace that movement with zero if the Lua
replacement returns no root displacement.

Mode132 does not enable the global 'player movement ignores animation' setting,
does not disable animation-driven locomotion generally, and does not alter
camera.getMode(). It only retains the already-computed player movement when all
of the following are true: FBFP locomotion compatibility is enabled, the actor
is the player, Lua animation handling is active, the player has a nonzero
movement state/vector, animation evaluation is not skipped, and the evaluated
animation returns exactly zero displacement. The Lua replacement continues to
play, preserving its visual leg/torso animation.
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

camera_hpp = (ROOT / "apps/openmw/mwrender/camera.hpp").read_text(encoding="utf-8")
camera_cpp = (ROOT / "apps/openmw/mwrender/camera.cpp").read_text(encoding="utf-8")
character = (ROOT / "apps/openmw/mwmechanics/character.cpp").read_text(encoding="utf-8")
defaults = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")

for marker in (
    "isFullBodyFirstPersonLocomotionCompat",
    "mV321FullBodyFirstPersonLocomotionCompat",
    "OPENMW_V321_CP4_LOCOMOTION_COMPAT",
):
    if marker not in camera_hpp + camera_cpp:
        raise RuntimeError(f"V3.21 CP4 locomotion camera switch incomplete: {marker}")
for marker in (
    "v321FullBodyLuaLocomotionFallback",
    "mLuaAnimations",
    "movementFromAnimation == osg::Vec3f()",
    "else if (!v321FullBodyLuaLocomotionFallback)",
):
    if marker not in character:
        raise RuntimeError(f"V3.21 CP4 locomotion CharacterController guard incomplete: {marker}")
if "v3.21 full body first person locomotion compatibility = false" not in defaults:
    raise RuntimeError("V3.21 CP4 locomotion compatibility is not default-off")
line131 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'131'"))
line132 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'132'"))
if "$V321CP4LocomotionCompat = '1'" in line131:
    raise RuntimeError("V3.21 CP4 locomotion contaminated Mode131")
for required in ("$V321CP3FullBodyFirstPerson = '1'", "$V321CP4ShadowCompat = '1'", "$V321CP4LocomotionCompat = '1'"):
    if required not in line132:
        raise RuntimeError(f"V3.21 Mode132 missing inherited/new switch: {required}")
if "openmw-custom-v3.21-cp4-locomotion-compat" not in (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8"):
    raise RuntimeError("V3.21 CP4 locomotion engine identity missing")

print("V3.21 CP4 full-body locomotion compatibility layer applied")
