#!/usr/bin/env python3
from pathlib import Path


def source(path: str) -> str:
    value = Path(path).read_text(encoding="utf-8")
    if not value:
        raise SystemExit(f"empty source file: {path}")
    return value


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        raise SystemExit(f"{label}: missing required source contract: {token}")


def forbid(text: str, token: str, label: str) -> None:
    if token in text:
        raise SystemExit(f"{label}: forbidden source contract present: {token}")


weapon_hpp = source("apps/openmw/mwrender/weaponanimation.hpp")
weapon_cpp = source("apps/openmw/mwrender/weaponanimation.cpp")
npc_hpp = source("apps/openmw/mwrender/npcanimation.hpp")
npc_cpp = source("apps/openmw/mwrender/npcanimation.cpp")
character_cpp = source("apps/openmw/mwmechanics/character.cpp")

# Preserve the gameplay-owned ranged/thrown pitch state as the primary factor.
require(weapon_hpp, "virtual float getAdditionalPitchFactor() const { return 0.f; }",
        "default pitch compatibility")
require(weapon_hpp, "float mAdditionalPitchBlend;", "procedural pitch blend state")
require(weapon_cpp, "float pitchFactor = mPitchFactor;", "gameplay pitch ownership")
require(weapon_cpp, "const float additionalPitchTarget = std::clamp(getAdditionalPitchFactor(), 0.f, 1.f);",
        "visual pitch target")
require(weapon_cpp, "MWBase::Environment::get().getFrameDuration()", "frame-rate independent pitch blending")
require(weapon_cpp, "constexpr float additionalBlendInPerSecond = 8.f;", "melee pitch blend-in")
require(weapon_cpp, "constexpr float additionalBlendOutPerSecond = 5.f;", "melee pitch blend-out")
require(weapon_cpp, "const float additionalPitchFactor = mAdditionalPitchBlend;",
        "smoothed pitch contribution")
require(weapon_cpp, "const bool usingAdditionalPitch = additionalPitchFactor > pitchFactor;",
        "non-destructive pitch composition")
require(weapon_cpp, "float pitch = characterPitchRadians * pitchFactor;", "composed spine pitch")
require(weapon_cpp, "constexpr float maxAdditionalPitchRadians = 0.4886921906f;",
        "procedural melee anti-clipping pitch cap")
require(weapon_cpp, "pitch = std::clamp(pitch, -maxAdditionalPitchRadians, maxAdditionalPitchRadians);",
        "procedural melee anti-clipping clamp")

# FFPB and normal third-person actors share the same third-person spine-controller skeleton path.
require(npc_cpp, "else if (mViewMode == VM_Normal || mViewMode == VM_FirstPersonFullBody)",
        "shared full-body controller route")
require(npc_cpp, "WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());",
        "shared spine controllers")

# The visual-only melee contribution is active for FFPB and ordinary third person,
# which deliberately includes NPCs/enemies, but remains attack-gated and melee-only.
require(npc_hpp, "mViewMode != VM_Normal && mViewMode != VM_FirstPersonFullBody",
        "third-person and FFPB view gate")
require(npc_hpp, "|| !mAccurateAiming", "active-attack gate")
require(npc_hpp, "weaponType == ESM::Weapon::None || weaponType == ESM::Weapon::Spell",
        "non-melee exclusion")
require(npc_hpp, "|| weaponType == ESM::Weapon::PickProbe", "tool exclusion")
require(npc_hpp, "mWeaponClass == ESM::WeaponType::Melee ? 1.f : 0.f",
        "melee-only additive pitch")

# The established ranged/thrown mechanics path remains intact. Melee pitch is added
# only in the render-animation layer, never by widening combat/mechanics pitch ownership.
require(character_cpp,
        "&& (weapclass == ESM::WeaponType::Ranged || weapclass == ESM::WeaponType::Thrown))",
        "ranged/thrown pitch mechanics")
forbid(character_cpp,
       "weapclass == ESM::WeaponType::Melee || weapclass == ESM::WeaponType::Ranged",
       "mechanics-owned melee pitch")

print("V4 procedural melee pitch animation contract: PASS")
