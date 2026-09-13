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
require(weapon_cpp, "float pitchFactor = mPitchFactor;", "gameplay pitch ownership")
require(weapon_cpp, "const float additionalPitchFactor = getAdditionalPitchFactor();",
        "view-specific pitch contribution")
require(weapon_cpp, "if (additionalPitchFactor > pitchFactor)", "non-destructive pitch composition")
require(weapon_cpp, "float pitch = characterPitchRadians * pitchFactor;", "composed spine pitch")

# Full-body first person must keep the existing third-person spine-controller skeleton path.
require(npc_cpp, "else if (mViewMode == VM_Normal || mViewMode == VM_FirstPersonFullBody)",
        "full-body controller route")
require(npc_cpp, "WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());",
        "shared spine controllers")

# The new contribution is deliberately limited to active FFPB melee attacks.
require(npc_hpp, "mViewMode != VM_FirstPersonFullBody || !mAccurateAiming",
        "FFPB active-attack gate")
require(npc_hpp, "weaponType == ESM::Weapon::None || weaponType == ESM::Weapon::Spell",
        "non-melee exclusion")
require(npc_hpp, "|| weaponType == ESM::Weapon::PickProbe", "tool exclusion")
require(npc_hpp, "mWeaponClass == ESM::WeaponType::Melee ? 1.f : 0.f",
        "melee-only additive pitch")

# The established bow/thrown mechanics path remains intact and third person must not be widened to melee.
require(character_cpp,
        "&& (weapclass == ESM::WeaponType::Ranged || weapclass == ESM::WeaponType::Thrown))",
        "ranged/thrown pitch mechanics")
forbid(character_cpp,
       "weapclass == ESM::WeaponType::Melee || weapclass == ESM::WeaponType::Ranged",
       "global third-person melee pitch")

print("V4 full-body first-person melee pitch contract: PASS")
