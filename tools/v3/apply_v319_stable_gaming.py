import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.19 stable match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.19 stable patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Stable gaming policy layer.
#
# This layer is deliberately additive over the exact validated V3.19 P0 source.
# It does not import P1/P1b instancing or shader changes. It only turns the
# promoted focus-cadence result into a first-class settings.cfg option and changes
# the normal/default policy from the historical causal-control value (1) to the
# promoted gaming value (2). OPENMW_V319_FOCUS_CADENCE remains a lab override.
# -----------------------------------------------------------------------------
focus_setting_anchor = '''        SettingValue<float> mV36FarCasterMinimumPixels{ mIndex, "V3", "v3.6 far caster minimum pixels",
            makeClampSanitizerFloat(0, 32) };'''
replace_exact(
    "components/settings/categories/cells.hpp",
    focus_setting_anchor,
    focus_setting_anchor
    + '''
        // V3.19 stable gaming: promoted focus temporal-coherence cadence.
        SettingValue<int> mV319FocusCadence{
            mIndex, "V3", "v3.19 focus cadence", makeClampSanitizerInt(1, 3) };''',
)

focus_default_anchor = "v3.6 disable coarse chunk occlusion = false"
replace_exact(
    "files/settings-default.cfg",
    focus_default_anchor,
    focus_default_anchor
    + '''

# V3.19 promoted normal-play focus refresh cadence. 1 reproduces the V3.18/P0
# control behavior; 2 is the validated stable gaming default; 3 remains available
# for manual experimentation. GUI mode still refreshes focus every frame.
v3.19 focus cadence = 2''',
)

old_focus = '''        static const unsigned v319FocusCadence = [] {
            const char* value = std::getenv("OPENMW_V319_FOCUS_CADENCE");
            if (value == nullptr || *value == '\\0')
                return 1u;
            const int parsed = std::atoi(value);
            return parsed >= 1 && parsed <= 3 ? static_cast<unsigned>(parsed) : 1u;
        }();'''
new_focus = '''        static const unsigned v319FocusCadence = [] {
            // Stable gaming reads the native setting by default. The environment
            // variable remains an explicit lab override so old benchmark modes keep
            // exact causal control without rewriting the user's settings.cfg.
            const unsigned configured = static_cast<unsigned>(Settings::cells().mV319FocusCadence);
            const char* value = std::getenv("OPENMW_V319_FOCUS_CADENCE");
            if (value == nullptr || *value == '\\0')
                return configured;
            const int parsed = std::atoi(value);
            return parsed >= 1 && parsed <= 3 ? static_cast<unsigned>(parsed) : configured;
        }();'''
replace_exact("apps/openmw/engine.cpp", old_focus, new_focus)

# Extend executable-bound provenance so a stable gaming artifact can never be
# confused with either the clean causal-control P0 package or the rejected P1 line.
replace_exact(
    "apps/openmw/engine.cpp",
    "openmw-custom-v3.19-cpu-p0",
    "openmw-custom-v3.19-cpu-p0 / openmw-custom-v3.19-p0-stable-gaming",
)

# Fail closed against accidentally contaminating this branch with the P1/P1b
# shader/instancing experiment that caused the semantic-control regression.
for rel in (
    "apps/openmw/engine.cpp",
    "files/shaders/compatibility/objects.vert",
    "files/shaders/compatibility/bs/default.vert",
    "files/shaders/compatibility/bs/nolighting.vert",
    "files/shaders/compatibility/shadowcasting.vert",
):
    text = (ROOT / rel).read_text(encoding="utf-8")
    for forbidden in ("v319StaticInstance", "OPENMW_V319_STATIC_INSTANCING"):
        if forbidden in text:
            raise RuntimeError(f"V3.19 stable P0 contamination: {forbidden} found in {rel}")

for rel, required in {
    "components/settings/categories/cells.hpp": (
        "mV319FocusCadence",
        '"V3", "v3.19 focus cadence"',
        "makeClampSanitizerInt(1, 3)",
    ),
    "files/settings-default.cfg": (
        "v3.19 focus cadence = 2",
        "validated stable gaming default",
    ),
    "apps/openmw/engine.cpp": (
        "Settings::cells().mV319FocusCadence",
        "OPENMW_V319_FOCUS_CADENCE",
        "openmw-custom-v3.19-p0-stable-gaming",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"V3.19 stable source missing {marker!r} in {rel}")

print("V3.19 clean-P0 stable gaming settings/policy layer applied")
