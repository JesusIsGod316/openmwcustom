import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

menu113 = [line for line in text.splitlines() if line.startswith("Write-Host '113 = V3.20 CP1")]
if len(menu113) != 1:
    raise RuntimeError(f"V3.20 CP2 menu113 anchor mismatch: {len(menu113)}")
extra_menu = "\n".join(
    (
        "Write-Host '114 = V3.20 CP2 exact P0 Lua control'",
        "Write-Host '115 = V3.20 CP2 engine event handler fastpaths only'",
        "Write-Host '116 = V3.20 CP2 pure sound ID/path conversion cache only'",
        "Write-Host '117 = V3.20 CP2 combined engine event + sound conversion fastpaths'",
    )
)
text = text.replace(menu113[0], menu113[0] + "\n" + extra_menu, 1)

text, count = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 113' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda match: "do { $choice = Read-Host 'Enter 1 through 117' } until ($choice -in @(" + match.group(1)
    + ",'114','115','116','117'))",
    text,
    count=1,
)
if count != 1:
    raise RuntimeError("V3.20 CP2 choice-range anchor mismatch")

default_anchor = """$V318RenderScaleManaged = 'false'
$V319FocusCadence = '1'
$V319OsgThreading = ''
$V320FocusAdaptive = '0'"""
if text.count(default_anchor) != 1:
    raise RuntimeError("V3.20 CP2 default anchor mismatch")
text = text.replace(
    default_anchor,
    default_anchor + "\n$V320EngineLuaFastPaths = '0'\n$V320SoundConversionCache = '0'",
    1,
)

line113 = next((line for line in text.splitlines() if line.lstrip().startswith("'113'")), None)
line109 = next((line for line in text.splitlines() if line.lstrip().startswith("'109'")), None)
if not line113 or not line109:
    raise RuntimeError("V3.20 CP2 mode anchors missing")
base_body = line109[line109.index("{") + 1 : line109.rindex("}")].strip()
if "v320-cp1-p0-control" not in base_body:
    raise RuntimeError("V3.20 CP2 expected CP1 P0 control body")


def mode_body(experiment: str, events: str, sound: str) -> str:
    body = base_body.replace("v320-cp1-p0-control", experiment, 1)
    return body + f"; $V320EngineLuaFastPaths = '{events}'; $V320SoundConversionCache = '{sound}'"


new_lines = (
    "        '114' { " + mode_body("v320-cp2-p0-control", "0", "0") + " }",
    "        '115' { " + mode_body("v320-cp2-engine-events", "1", "0") + " }",
    "        '116' { " + mode_body("v320-cp2-sound-conversion", "0", "1") + " }",
    "        '117' { " + mode_body("v320-cp2-combined", "1", "1") + " }",
)
anchor = line113 + "\n"
if text.count(anchor) != 1:
    raise RuntimeError("V3.20 CP2 mode insertion anchor mismatch")
text = text.replace(anchor, line113 + "\n" + "\n".join(new_lines) + "\n", 1)

manifest_anchor = '    "v320_focus_adaptive=$V320FocusAdaptive",'
if text.count(manifest_anchor) != 1:
    raise RuntimeError("V3.20 CP2 manifest anchor mismatch")
text = text.replace(
    manifest_anchor,
    manifest_anchor
    + '\n    "v320_engine_lua_fastpaths=$V320EngineLuaFastPaths",'
    + '\n    "v320_sound_conversion_cache=$V320SoundConversionCache",',
    1,
)

launch_anchor = "    $env:OPENMW_V320_FOCUS_ADAPTIVE = $V320FocusAdaptive"
if text.count(launch_anchor) != 1:
    raise RuntimeError("V3.20 CP2 launch anchor mismatch")
text = text.replace(
    launch_anchor,
    launch_anchor
    + "\n    $env:OPENMW_V320_ENGINE_LUA_FASTPATHS = $V320EngineLuaFastPaths"
    + "\n    $env:OPENMW_V320_SOUND_CONVERSION_CACHE = $V320SoundConversionCache",
    1,
)

finally_anchor = "finally {\n    Remove-Item Env:OPENMW_V320_FOCUS_ADAPTIVE -ErrorAction SilentlyContinue"
if text.count(finally_anchor) != 1:
    raise RuntimeError("V3.20 CP2 finally anchor mismatch")
text = text.replace(
    finally_anchor,
    "finally {\n"
    "    Remove-Item Env:OPENMW_V320_ENGINE_LUA_FASTPATHS -ErrorAction SilentlyContinue\n"
    "    Remove-Item Env:OPENMW_V320_SOUND_CONVERSION_CACHE -ErrorAction SilentlyContinue\n"
    "    Remove-Item Env:OPENMW_V320_FOCUS_ADAPTIVE -ErrorAction SilentlyContinue",
    1,
)

for marker in (
    "Enter 1 through 117",
    "v320-cp2-p0-control",
    "v320-cp2-engine-events",
    "v320-cp2-sound-conversion",
    "v320-cp2-combined",
    "OPENMW_V320_ENGINE_LUA_FASTPATHS",
    "OPENMW_V320_SOUND_CONVERSION_CACHE",
):
    if marker not in text:
        raise RuntimeError(f"V3.20 CP2 launcher missing marker: {marker}")

launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.20 CP2 causal Lua modes 114-117 added")
